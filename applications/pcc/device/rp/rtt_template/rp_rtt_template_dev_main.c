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
#include "pcc_common_dev.h"
#include "rtt_template.h"

#define DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK (1 << DOCA_PCC_DEV_EVNT_ROCE_ACK)
#define SAMPLER_THREAD_RANK (0)
#define COUNTERS_SAMPLE_WINDOW_IN_MICROSEC (10)

/**< Counters IDs to configure and read from */
uint32_t counter_ids[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Table of TX bytes counters to sample to */
uint32_t current_sampled_tx_bytes[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Table of TX bytes counters that was last sampled */
uint32_t previous_sampled_tx_bytes[DOCA_PCC_DEV_MAX_NUM_PORTS] = {0};
/**< Last timestamp of sampled counters */
uint32_t last_sample_ts;
/**< Ports active bandwidth. Units of MB/s */
uint32_t ports_bw[DOCA_PCC_DEV_MAX_NUM_PORTS];
/**< Number of available and initiated logical ports */
uint32_t ports_num = 0;
/**< Percentage of the current active ports utilized bandwidth. Saved in FXP 16 format */
uint32_t g_utilized_bw[DOCA_PCC_DEV_MAX_NUM_PORTS];
/**< Flag to indicate that the counters have been initiated */
uint32_t counters_started = 0;

#ifdef DOCA_PCC_SAMPLE_TX_BYTES
/*
 * Dedicate one thread to sample tx bytes counters on a defined time frame,
 * calculate current bandwidth and compare with maximum port bandwidth.
 * This call is enabled by user option to sample TX bytes counter
 */
FORCE_INLINE void thread0_calc_ports_utilization(void)
{
	uint32_t tx_bytes_delta[DOCA_PCC_DEV_MAX_NUM_PORTS], current_bw[DOCA_PCC_DEV_MAX_NUM_PORTS], ts_delta,
		current_ts;

	if ((doca_pcc_dev_thread_rank() == SAMPLER_THREAD_RANK) && counters_started) {
		current_ts = doca_pcc_dev_get_timer_lo();
		ts_delta = diff_with_wrap32(current_ts, last_sample_ts);
		if (ts_delta >= COUNTERS_SAMPLE_WINDOW_IN_MICROSEC) {
			doca_pcc_dev_nic_counters_sample();
			for (uint32_t i = 0; i < ports_num; i++) {
				tx_bytes_delta[i] =
					diff_with_wrap32(current_sampled_tx_bytes[i], previous_sampled_tx_bytes[i]);
				previous_sampled_tx_bytes[i] = current_sampled_tx_bytes[i];
				current_bw[i] =
					(doca_pcc_dev_fxp_mult(tx_bytes_delta[i], doca_pcc_dev_fxp_recip(ts_delta)) >>
					 16);
				g_utilized_bw[i] = (1 << 16);
				if (current_bw[i] < ports_bw[i])
					g_utilized_bw[i] = doca_pcc_dev_fxp_mult(current_bw[i],
										 doca_pcc_dev_fxp_recip(ports_bw[i]));
			}
			last_sample_ts = current_ts;
		}
	}
}

/**
 * @brief Count the number of available logical ports from queried mask
 *
 * @param[in] ports_mask - ports_mask
 *
 * @return - number of available logical ports initiated in mask
 */
FORCE_INLINE uint32_t count_ports(uint32_t ports_mask)
{
	// find maximum port id enabled. Assume enabled ports are continuous
	return doca_pcc_dev_fls(ports_mask);
}

/**
 * @brief Initiate counter IDs global array on port for TX bytes counter type
 */
FORCE_INLINE void init_counter_ids(void)
{
	for (uint32_t i = 0; i < DOCA_PCC_DEV_MAX_NUM_PORTS; i++)
		counter_ids[i] = DOCA_PCC_DEV_GET_PORT_COUNTER_ID(i, DOCA_PCC_DEV_NIC_COUNTER_TYPE_TX_BYTES, 0);
}

/*
 * Initialize TX counters sampling
 */
FORCE_INLINE void tx_counters_sampling_init(uint32_t portid)
{
	/* number of ports to initiate counters for */
	ports_num = count_ports(doca_pcc_dev_get_logical_ports());
	/* Configure counters to read */
	doca_pcc_dev_nic_counters_config(counter_ids, ports_num, current_sampled_tx_bytes);
	/* save port speed in MBps units */
	ports_bw[portid] = (doca_pcc_dev_mult(doca_pcc_dev_get_port_speed(portid), 1000) >> 3);
	/* Sample counters and save in global table */
	doca_pcc_dev_nic_counters_sample();
	last_sample_ts = doca_pcc_dev_get_timer_lo();
	/* Save sampled TX bytes */
	for (uint32_t i = 0; i < ports_num; i++)
		previous_sampled_tx_bytes[i] = current_sampled_tx_bytes[i];
	counters_started = 1;
}

/*
 * Called on link or port info state change.
 * This callback is used to configure port counters to query TX bytes on
 *
 * @return - void
 */
void doca_pcc_dev_user_port_info_changed(uint32_t portid)
{
	tx_counters_sampling_init(portid);
}
#endif

/*
 * Main entry point to user CC algorithm (Reference code)
 * This function starts the algorithm code of a single event
 * It receives the flow context data, the event info and outputs the new rate parameters
 * The function can support multiple algorithms and can call the per algorithm handler based on
 * the algo type. If a single algorithm is required this code can be simplified
 * The function can not be renamed as it is called by the handler infrastructure
 *
 * @algo_ctxt [in]: A pointer to a flow context data retrieved by libpcc.
 * @event [in]: A pointer to an event data structure to be passed to extractor functions
 * @attr [in]: A pointer to additional parameters (algo type).
 * @results [out]: A pointer to result struct to update rate in HW.
 */
void doca_pcc_dev_user_algo(doca_pcc_dev_algo_ctxt_t *algo_ctxt,
			    doca_pcc_dev_event_t *event,
			    const doca_pcc_dev_attr_t *attr,
			    doca_pcc_dev_results_t *results)
{
	uint32_t port_num = doca_pcc_dev_get_ev_attr(event).port_num;
	uint32_t *param = doca_pcc_dev_get_algo_params(port_num, attr->algo_slot);
	uint32_t *counter = doca_pcc_dev_get_counters(port_num, attr->algo_slot);

#ifdef DOCA_PCC_SAMPLE_TX_BYTES
	thread0_calc_ports_utilization();
#endif

	switch (attr->algo_slot) {
	case 0: {
		rtt_template_algo(event, param, counter, algo_ctxt, results);
		break;
	}
	default: {
		doca_pcc_dev_default_internal_algo(algo_ctxt, event, attr, results);
		break;
	}
	};
}

/*
 * Main entry point to user algorithm initialization (reference code)
 * This function starts the user algorithm initialization code
 * The function will be called once per process load and should init all supported
 * algorithms and all ports
 *
 * @disable_event_bitmask [out]: user code can tell the infrastructure which event
 * types to ignore (mask out). Events of this type will be dropped and not passed to
 * any algo
 */
void doca_pcc_dev_user_init(uint32_t *disable_event_bitmask)
{
	uint32_t algo_idx = 0, algo_slot = 0, algo_en = 1;

	/* Initialize algorithm with algo_idx=0 */
	rtt_template_init(algo_idx);

	for (int port_num = 0; port_num < DOCA_PCC_DEV_MAX_NUM_PORTS; ++port_num) {
		/* Slot 0 will use algo_idx 0, default enabled */
		doca_pcc_dev_init_algo_slot(port_num, algo_slot, algo_idx, algo_en);
		doca_pcc_dev_trace_5(0, port_num, algo_idx, algo_slot, algo_en, DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK);
	}

#ifdef DOCA_PCC_SAMPLE_TX_BYTES
	/** Assuming this is called prior to doca_pcc_dev_user_port_info_changed() */
	init_counter_ids();
#endif

	/* disable events of below type */
	*disable_event_bitmask = DOCA_PCC_DEV_EVNT_ROCE_ACK_MASK;
	if (DOCA_PCC_DEV_ACK_NACK_TX_EVENT_DISABLED_SUPPORTED == 1) {
		*disable_event_bitmask |= (1 << DOCA_PCC_DEV_EVNT_ROCE_TX_FOR_ACK_NACK);
	}

	doca_pcc_dev_printf("%s, disable_event_bitmask=0x%x\n", __func__, *disable_event_bitmask);
	doca_pcc_dev_trace_flush();
}

/*
 * Called when the parameter change was set externally.
 * The implementation should:
 *     Check the given new_parameters values. If those are correct from the algorithm perspective,
 *     assign them to the given parameter array.

 * @port_num [in]: index of the port
 * @algo_slot [in]: Algo slot identifier as referred to in the PPCC command field "algo_slot"
 * if possible it should be equal to the algo_idx
 * @param_id_base [in]: id of the first parameter that was changed.
 * @param_num [in]: number of all parameters that were changed
 * @new_param_values [in]: pointer to an array which holds param_num number of new values for parameters
 * @params [in]: pointer to an array which holds beginning of the current parameters to be changed
 * @return -
 * DOCA_PCC_DEV_STATUS_OK: Parameters were set
 * DOCA_PCC_DEV_STATUS_FAIL: the values (one or more) are not legal. No parameters were changed
 */
doca_pcc_dev_error_t doca_pcc_dev_user_set_algo_params(uint32_t port_num,
						       uint32_t algo_slot,
						       uint32_t param_id_base,
						       uint32_t param_num,
						       const uint32_t *new_param_values,
						       uint32_t *params)
{
	/* Notify the user that a change happened to take action.
	 * I.E.: Pre calculate values to be used in the algo that are based on the parameter value.
	 * Support more complex checks. E.G.: Param is a bit mask - min and max do not help
	 * Param dependency checking.
	 */
	doca_pcc_dev_error_t ret = DOCA_PCC_DEV_STATUS_OK;

	switch (algo_slot) {
	case 0: {
		uint32_t algo_idx = doca_pcc_dev_get_algo_index(port_num, algo_slot);

		if (algo_idx == 0)
			ret = rtt_template_set_algo_params(param_id_base, param_num, new_param_values, params);
		else
			ret = DOCA_PCC_DEV_STATUS_FAIL;

		break;
	}
	default:
		break;
	}
	return ret;
}
