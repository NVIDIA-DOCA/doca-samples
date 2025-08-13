/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_pcc_dev.h>
#include <doca_pcc_dev_event.h>
#include <doca_pcc_dev_algo_access.h>

#include "utils.h"
#include "rtt_template_ctxt.h"
#include "rtt_template_algo_params.h"
#include "rtt_template.h"

#pragma clang diagnostic ignored "-Wunused-parameter"

/* Algorithm parameters are defined in rtt_template_algo_params.h */

/* Define the constants */
#define FF32BIT (0xffffffff)
#define DEC_FACTOR ((1 << 16) - param[RTT_TEMPLATE_UPDATE_FACTOR])	    /* Rate decrease factor */
#define CNP_DEC_FACTOR ((1 << 16) - 2 * param[RTT_TEMPLATE_UPDATE_FACTOR])  /* CNP rate decrease factor */
#define NACK_DEC_FACTOR ((1 << 16) - 5 * param[RTT_TEMPLATE_UPDATE_FACTOR]) /* NACK rate decrease factor */
#define ABORT_TIME (300000)						    /* The time to abort rtt_req - in nanosec */
#define BW_MB_DEFAULT_FXP16 (BW_MB_DEFAULT << 16)			    /* Default BW in fixed point */
#define NP_RX_RATE_TH (58982) /* Threshold to update current rate according to NP RX rate. 0.9 in 16b fixed point */
#define HIGH_UTIL_THRESHOLD (55704) /* 0.85 in 16b fixed point */
#define HIGH_UTIL_DEC_FACTOR \
	((1 << 16) - 2 * param[RTT_TEMPLATE_UPDATE_FACTOR]) /* Rate decrease factor in high port utilization mode */
#define HIGH_UTIL_CNP_DEC_FACTOR \
	((1 << 16) - 4 * param[RTT_TEMPLATE_UPDATE_FACTOR]) /* CNP rate decrease factor in high port utilization mode \
							     */
#define HIGH_UTIL_NACK_DEC_FACTOR \
	((1 << 16) - 10 * param[RTT_TEMPLATE_UPDATE_FACTOR]) /* NACK rate decrease factor in high port utilization \
								mode */

typedef enum {
	RTT_TEMPLATE_UPDATE_FACTOR = 0, /* configurable parameter of update factor */
	RTT_TEMPLATE_AI = 1,		/* configurable parameter of AI */
	RTT_TEMPLATE_BASE_RTT = 2,	/* configurable parameter of base rtt */
	RTT_TEMPLATE_NEW_FLOW_RATE = 3, /* configurable parameter of new flow rate */
	RTT_TEMPLATE_MIN_RATE = 4,	/* configurable parameter of min rate */
	RTT_TEMPLATE_MAX_DELAY = 5,	/* configurable parameter of max delay */
	RTT_TEMPLATE_PARAM_NUM		/* Maximal number of configurable parameters */
} rtt_template_params_t;

enum {
	RTT_TEMPLATE_COUNTER_TX_EVENT = 0,  /* tx event for rtt template user algorithm */
	RTT_TEMPLATE_COUNTER_RTT_EVENT = 1, /* rtt event for rtt template user algorithm */
	RTT_TEMPLATE_COUNTER_NUM	    /* Maximal number of counters */
} rtt_template_counter_t;

const volatile char rtt_template_desc[] = "Rtt template v0.1";
static const volatile char rtt_template_param_update_factor_desc[] = "UPDATE_FACTOR, update factor";
static const volatile char rtt_template_param_ai_desc[] = "AI, ai";
static const volatile char rtt_template_param_base_rtt_desc[] = "BASE_RTT, base rtt";
static const volatile char rtt_template_param_new_flow_rate_desc[] = "NEW_FLOW_RATE, new flow rate";
static const volatile char rtt_template_param_min_rate_desc[] = "MIN_RATE, min rate";
static const volatile char rtt_template_param_max_delay_desc[] = "MAX_DELAY, max delay";
static const volatile char rtt_template_counter_tx_desc[] = "COUNTER_TX_EVENT, number of tx events handled";
static const volatile char rtt_template_counter_rtt_desc[] = "COUNTER_RTT_EVENT, number of rtt events handled";

void rtt_template_init(uint32_t algo_idx)
{
	struct doca_pcc_dev_algo_meta_data algo_def = {0};

	algo_def.algo_id = 0xBFFF;
	algo_def.algo_major_version = 0x00;
	algo_def.algo_minor_version = 0x01;
	algo_def.algo_desc_size = sizeof(rtt_template_desc);
	algo_def.algo_desc_addr = (uint64_t)rtt_template_desc;

	uint32_t total_param_num = RTT_TEMPLATE_PARAM_NUM;
	uint32_t total_counter_num = RTT_TEMPLATE_COUNTER_NUM;
	uint32_t param_num = 0;
	uint32_t counter_num = 0;

	doca_pcc_dev_algo_init_metadata(algo_idx, &algo_def, total_param_num, total_counter_num);

	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     UPDATE_FACTOR,
				     UPDATE_FACTOR_MAX,
				     1,
				     1,
				     sizeof(rtt_template_param_update_factor_desc),
				     (uint64_t)rtt_template_param_update_factor_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     AI,
				     AI_MAX,
				     1,
				     1,
				     sizeof(rtt_template_param_ai_desc),
				     (uint64_t)rtt_template_param_ai_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     BASE_RTT,
				     UINT32_MAX,
				     1,
				     1,
				     sizeof(rtt_template_param_base_rtt_desc),
				     (uint64_t)rtt_template_param_base_rtt_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     NEW_FLOW_RATE,
				     RATE_MAX,
				     1,
				     1,
				     sizeof(rtt_template_param_new_flow_rate_desc),
				     (uint64_t)rtt_template_param_new_flow_rate_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     MIN_RATE,
				     RATE_MAX,
				     1,
				     1,
				     sizeof(rtt_template_param_min_rate_desc),
				     (uint64_t)rtt_template_param_min_rate_desc);
	doca_pcc_dev_algo_init_param(algo_idx,
				     param_num++,
				     MAX_DELAY,
				     UINT32_MAX,
				     1,
				     1,
				     sizeof(rtt_template_param_max_delay_desc),
				     (uint64_t)rtt_template_param_max_delay_desc);

	doca_pcc_dev_algo_init_counter(algo_idx,
				       counter_num++,
				       UINT32_MAX,
				       2,
				       sizeof(rtt_template_counter_tx_desc),
				       (uint64_t)rtt_template_counter_tx_desc);
	doca_pcc_dev_algo_init_counter(algo_idx,
				       counter_num++,
				       UINT32_MAX,
				       2,
				       sizeof(rtt_template_counter_rtt_desc),
				       (uint64_t)rtt_template_counter_rtt_desc);
}

/*
 * Entry point to core function of algorithm (reference code)
 * This function adjusts rate based on CC events.
 * It calculates the new rate parameters based on flow context data, rtt info and current rate.
 *
 * @ccctx [in]: A pointer to a flow context data retrieved by libpcc.
 * @rtt [in]: The value of rtt.
 * @cur_rate [in]: Current rate value
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior
 * @is_high_util [in]: Flag to indicate if the port is in high utilization mode
 * @norm_np_rx_rate [in]: Notification Point RX rate normalized in fxp
 * @return: The new calculated rate value
 */
static inline uint32_t algorithm_core(cc_ctxt_rtt_template_t *ccctx,
				      uint32_t rtt,
				      uint32_t cur_rate,
				      uint32_t *param,
				      uint8_t is_high_util,
				      uint32_t norm_np_rx_rate)
{
	/* ##### Put your algorithm code in here #### */

	/* Example */
	if (ccctx->flags.was_nack && (rtt >= param[RTT_TEMPLATE_MAX_DELAY])) {
		/* NACK */
		cur_rate = doca_pcc_dev_fxp_mult(is_high_util ? HIGH_UTIL_NACK_DEC_FACTOR : NACK_DEC_FACTOR, cur_rate);
		ccctx->flags.was_nack = 0;
	} else if (ccctx->flags.was_cnp || (rtt >= param[RTT_TEMPLATE_MAX_DELAY])) {
		/* CNP */
		cur_rate = doca_pcc_dev_fxp_mult(is_high_util ? HIGH_UTIL_CNP_DEC_FACTOR : CNP_DEC_FACTOR, cur_rate);
		ccctx->flags.was_cnp = 0;
	} else {
		/* RTT */
		if (rtt > param[RTT_TEMPLATE_BASE_RTT])
			cur_rate = doca_pcc_dev_fxp_mult(is_high_util ? HIGH_UTIL_DEC_FACTOR : DEC_FACTOR, cur_rate);
		else {
			cur_rate += param[RTT_TEMPLATE_AI];
		}
	}

	if (cur_rate > DOCA_PCC_DEV_MAX_RATE)
		cur_rate = DOCA_PCC_DEV_MAX_RATE;

	if (cur_rate < param[RTT_TEMPLATE_MIN_RATE])
		cur_rate = param[RTT_TEMPLATE_MIN_RATE];
	/* End of example */

	return cur_rate;
}

/*
 * Entry point to rtt template to handle roce tx event (reference code)
 * This function updates flags for rtt measurement or re-send rtt_req if needed.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void rtt_template_handle_roce_tx(doca_pcc_dev_event_t *event,
					       uint32_t cur_rate,
					       cc_ctxt_rtt_template_t *ccctx,
					       doca_pcc_dev_results_t *results)
{
	uint8_t rtt_req = 0;
	uint32_t rtt_meas_psn = ccctx->rtt_meas_psn;
	uint32_t timestamp = doca_pcc_dev_get_timestamp(event);
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);

	if (unlikely((ev_attr.flags & DOCA_PCC_DEV_TX_FLAG_RTT_REQ_SENT) && (rtt_meas_psn == 0))) {
		ccctx->rtt_meas_psn = 1;
		ccctx->rtt_req_to_rtt_sent = 0;
		ccctx->start_delay = timestamp;
	} else {
		/* Calculate rtt_till_now */
		uint32_t rtt_till_now = (timestamp - ccctx->start_delay);

		if (unlikely(ccctx->start_delay > timestamp))
			rtt_till_now += UINT32_MAX;
		/* Abort RTT request flow - for cases event or packet was dropped */
		if (rtt_meas_psn == 0) {
			rtt_till_now = 0;
			ccctx->rtt_req_to_rtt_sent += 1;
		}
		if (unlikely((rtt_till_now > ((uint32_t)ABORT_TIME << ccctx->abort_cnt)) ||
			     (ccctx->rtt_req_to_rtt_sent > 2))) {
			rtt_req = 1;
			if (rtt_till_now > ((uint32_t)ABORT_TIME << ccctx->abort_cnt))
				ccctx->abort_cnt += 1;
			ccctx->rtt_req_to_rtt_sent = 1;
		}
	}

	/* Update results buffer and context */
	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = rtt_req;
}

/*
 * Entry point to rtt template to handle roce rtt event (reference code)
 * This function calculates the rtt and calls core function to adjust rate.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void rtt_template_handle_roce_rtt(doca_pcc_dev_event_t *event,
						uint32_t cur_rate,
						uint32_t *param,
						cc_ctxt_rtt_template_t *ccctx,
						doca_pcc_dev_results_t *results)
{
	/*
	 * Check that this RTT event is the one we are waiting for.
	 * For cases we re-send RTT request by mistake due to abort flow for example.
	 */
	uint32_t rtt_meas_psn = ccctx->rtt_meas_psn;

	if (unlikely(((rtt_meas_psn == 0) && (ccctx->rtt_req_to_rtt_sent == 0)))) {
		results->rate = cur_rate;
		results->rtt_req = 0;
		return;
	}

	/* We got RTT measurement */
	/* Reset variables */
	ccctx->rtt_meas_psn = 0;
	ccctx->abort_cnt = 0;

	/* RTT calculation */
	uint32_t start_rtt = doca_pcc_dev_get_rtt_req_send_timestamp(event);
#ifdef TIME_SYNC
	/* if there is a time sync between the nodes we can get one way delay */
	uint32_t end_rtt = doca_pcc_dev_get_rtt_req_recv_timestamp(event);
#else
	uint32_t end_rtt = doca_pcc_dev_get_timestamp(event);
#endif /* TIME_SYNC */
	int32_t rtt = end_rtt - start_rtt;

	if (unlikely(end_rtt < start_rtt))
		rtt += UINT32_MAX;
	ccctx->rtt = rtt;

	/* Call to the core of the CC algorithm */

#ifdef DOCA_PCC_SAMPLE_TX_BYTES
	uint8_t is_high_tx_util =
		(rtt_get_last_tx_port_util(doca_pcc_dev_get_ev_attr(event).port_num) > HIGH_UTIL_THRESHOLD);
#else
	uint8_t is_high_tx_util = 0;
#endif

	uint32_t norm_np_rx_rate = (1 << 16);

	cur_rate = algorithm_core(ccctx, rtt, cur_rate, param, is_high_tx_util, norm_np_rx_rate);

	ccctx->rtt_req_to_rtt_sent = 1;
	ccctx->cur_rate = cur_rate;
	results->rate = cur_rate;
	results->rtt_req = 1;
}

/*
 * Entry point to rtt template to handle roce cnp events (reference code)
 * This function puts the code for immediate reaction to CNPs.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void rtt_template_handle_roce_cnp(doca_pcc_dev_event_t *event,
						uint32_t cur_rate,
						cc_ctxt_rtt_template_t *ccctx,
						doca_pcc_dev_results_t *results)
{
	ccctx->flags.was_cnp = 1;

	/* ###### You can put the code for immediate reaction to CNPs ####### */
	/*
	 * e.g:
	 * cur_rate =
	 */
	results->rtt_req = 0;
	results->rate = cur_rate;
	ccctx->cur_rate = cur_rate;
}

/*
 * Entry point to rtt template to handle roce nack events (reference code)
 * This function puts the code for immediate reaction to NACKs.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @ccctx [in]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void rtt_template_handle_roce_nack(doca_pcc_dev_event_t *event,
						 uint32_t cur_rate,
						 cc_ctxt_rtt_template_t *ccctx,
						 doca_pcc_dev_results_t *results)
{
	ccctx->flags.was_nack = 1;
	results->rate = cur_rate;
	ccctx->cur_rate = cur_rate;
}

/*
 * Entry point to rtt template to handle new flow (reference code)
 * This function initializes the flow context.
 * It calculates the new rate parameters based on flow context data, event info and current rate.
 *
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @cur_rate [in]: Current rate value
 * @param [in]: A pointer to an array of parameters that are used to control algo behavior
 * @ccctx [in/out]: A pointer to a flow context data retrieved by libpcc.
 * @results [out]: A pointer to result struct to update rate in HW.
 */
static inline void rtt_template_handle_new_flow(doca_pcc_dev_event_t *event,
						uint32_t cur_rate,
						uint32_t *param,
						cc_ctxt_rtt_template_t *ccctx,
						doca_pcc_dev_results_t *results)
{
	ccctx->cur_rate = param[RTT_TEMPLATE_NEW_FLOW_RATE];
	ccctx->start_delay = doca_pcc_dev_get_timestamp(event);
	ccctx->rtt_meas_psn = 0;
	ccctx->rtt_req_to_rtt_sent = 1;
	ccctx->abort_cnt = 0;
	ccctx->flags.was_nack = 0;
	results->rate = param[RTT_TEMPLATE_NEW_FLOW_RATE];
	results->rtt_req = 1;
}

void rtt_template_algo(doca_pcc_dev_event_t *event,
		       uint32_t *param,
		       uint32_t *counter,
		       doca_pcc_dev_algo_ctxt_t *algo_ctxt,
		       doca_pcc_dev_results_t *results)
{
	cc_ctxt_rtt_template_t *rtt_template_ctx = (cc_ctxt_rtt_template_t *)algo_ctxt;
	doca_pcc_dev_event_general_attr_t ev_attr = doca_pcc_dev_get_ev_attr(event);
	uint32_t ev_type = ev_attr.ev_type;
	uint32_t cur_rate = rtt_template_ctx->cur_rate;

	if (unlikely(cur_rate == 0)) {
		rtt_template_handle_new_flow(event, cur_rate, param, rtt_template_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_TX) {
		rtt_template_handle_roce_tx(event, cur_rate, rtt_template_ctx, results);
		/* Example code to update counter */
		if (counter != NULL)
			counter[RTT_TEMPLATE_COUNTER_TX_EVENT]++;
	} else if (ev_type == DOCA_PCC_DEV_EVNT_RTT) {
		rtt_template_handle_roce_rtt(event, cur_rate, param, rtt_template_ctx, results);
		/* Example code to update counter */
		if (counter != NULL)
			counter[RTT_TEMPLATE_COUNTER_RTT_EVENT]++;
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_CNP) {
		rtt_template_handle_roce_cnp(event, cur_rate, rtt_template_ctx, results);
	} else if (ev_type == DOCA_PCC_DEV_EVNT_ROCE_NACK) {
		rtt_template_handle_roce_nack(event, cur_rate, rtt_template_ctx, results);
	} else {
		results->rate = cur_rate;
		results->rtt_req = 0;
	}
}

doca_pcc_dev_error_t rtt_template_set_algo_params(uint32_t param_id_base,
						  uint32_t param_num,
						  const uint32_t *new_param_values,
						  uint32_t *params)
{
	/* Example */
	if ((param_num > RTT_TEMPLATE_PARAM_NUM) || (param_id_base >= RTT_TEMPLATE_PARAM_NUM))
		return DOCA_PCC_DEV_STATUS_FAIL;

	if ((new_param_values == NULL) || (params == NULL))
		return DOCA_PCC_DEV_STATUS_FAIL;

	return DOCA_PCC_DEV_STATUS_OK;
	/* End of example */
}
