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

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <rte_ethdev.h>
#include <rte_hash_crc.h>

#include <dpdk_utils.h>
#include <doca_dpdk.h>
#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_bitfield.h>
#include <doca_log.h>

#include "upf_accel.h"
#include "upf_accel_flow_processing.h"
#include "upf_accel_pipeline.h"

DOCA_LOG_REGISTER(UPF_ACCEL);

#define UPF_ACCEL_RSS_KEY_LEN 40

volatile bool force_quit;

static uint8_t upf_accel_rss_hash_key[UPF_ACCEL_RSS_KEY_LEN] = {
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
};

/*
 * Calculate quota counter index according to a port id and zero based index
 *
 * @port_id [in]: port id
 * @idx [in]: relative (zero based) index
 * @return: absolute (port wise) quota counter index
 */
static inline uint32_t port_id_and_idx_to_quota_counter(enum upf_accel_port port_id, uint32_t idx)
{
	return port_id * UPF_ACCEL_NUM_QUOTA_COUNTERS_PER_PORT + idx;
}

/*
 * Get the offset of a meter.
 *
 * Meters are organized as follows:
 *
 *         --       --
 *         |        | Meter[0]
 *         |        | Meter[1]
 *         | PDR[0]- ...
 *         |        |
 *         |        | Meter[UPF_ACCEL_MAX_PDR_NUM_RATE_METERS - 1]
 * Port[0]-         --
 *         |        | Meter[UPF_ACCEL_MAX_PDR_NUM_RATE_METERS]
 *         |        | Meter[UPF_ACCEL_MAX_PDR_NUM_RATE_METERS +1]
 *         | PDR[1] - ...
 *         |        |
 *         |        |
 *         --       -
 *...
 *
 * @port_id [in]: port ID .
 * @pdr_idx [in]: PDR index.
 * @meter_idx [in]: meter index.
 * @return: offset in meter table.
 */
static inline uint32_t upf_accel_shared_meters_table_offset_get(enum upf_accel_port port_id,
								uint32_t pdr_idx,
								uint32_t meter_idx)
{
	const uint32_t num_meters_per_port = UPF_ACCEL_MAX_PDR_NUM_RATE_METERS * UPF_ACCEL_MAX_NUM_PDR;

	return (port_id * num_meters_per_port) + UPF_ACCEL_MAX_PDR_NUM_RATE_METERS * pdr_idx + meter_idx;
}

/*
 * Clamp rate
 *
 * @val [in]: rate.
 * @return: rate clamped to [1, inf]
 */
static inline uint64_t upf_accel_clamp_rate(uint64_t val)
{
	return val > 0 ? val : 1;
}

/*
 * Returns the index of a given pdr pointer located in pdrs.
 *
 * @pdrs [in]: PDRs struct
 * @pdr [in]: PDR pointer from PDRs struct
 * @return: pdr index in pdrs
 */
static inline uint32_t upf_accel_get_pdr_index_from_pdrs(const struct upf_accel_pdrs *pdrs,
							 const struct upf_accel_pdr *pdr)
{
	if ((pdr < pdrs->arr_pdrs) || (pdr >= pdrs->arr_pdrs + pdrs->num_pdrs)) {
		DOCA_LOG_ERR("Given PDR is not part of pdrs.");
		assert(0);
	}

	return pdr - pdrs->arr_pdrs;
}

/*
 * Returns QER by a given QER ID.
 *
 * @qers [in]: QERs struct
 * @qer_id [in]: QER ID
 * @return: qer pointer in qers
 */
static inline struct upf_accel_qer *upf_accel_get_qer_by_qer_id(struct upf_accel_qers *qers, uint32_t qer_id)
{
	uint32_t i;

	for (i = 0; i < qers->num_qers; ++i) {
		if (qers->arr_qers[i].id == qer_id) {
			return &qers->arr_qers[i];
		}
	}

	DOCA_LOG_ERR("Failed to find qer ID %u", qer_id);
	assert(0);

	return qers->arr_qers;
}

/*
 * Shared meters init for a given port
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @cfg [in]: shared resource configuration.
 * @qer [in]: UPF PDR
 * @port_id [in]: port ID.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_shared_meters_dev_init(struct upf_accel_ctx *upf_accel_ctx,
						     struct doca_flow_shared_resource_cfg *cfg,
						     const struct upf_accel_pdr *pdr,
						     enum upf_accel_port port_id)
{
	struct doca_flow_port *port = upf_accel_ctx->ports[port_id];
	struct upf_accel_qers *qers = upf_accel_ctx->upf_accel_cfg->qers;
	uint32_t ids_array[UPF_ACCEL_MAX_NUM_PDR] = {0};
	struct upf_accel_qer *qer;
	uint64_t ul_cir_cbs;
	uint64_t dl_cir_cbs;
	doca_error_t result;
	uint32_t meter_idx;
	uint32_t i;

	for (i = 0; i < pdr->qerids_num; ++i) {
		qer = upf_accel_get_qer_by_qer_id(qers, pdr->qerids[i]);

		/*
		 * QER MBR units: 1 kilobit per second.
		 * CBS units: 1 byte per second.
		 */
		ul_cir_cbs = upf_accel_clamp_rate(1000 * (qer->mbr_ul_mbr / CHAR_BIT));
		dl_cir_cbs = upf_accel_clamp_rate(1000 * (qer->mbr_dl_mbr / CHAR_BIT));

		meter_idx = upf_accel_shared_meters_table_offset_get(
			port_id,
			upf_accel_get_pdr_index_from_pdrs(upf_accel_ctx->upf_accel_cfg->pdrs, pdr),
			i);
		cfg->meter_cfg.cir = cfg->meter_cfg.cbs = (pdr->pdi_si == UPF_ACCEL_PDR_PDI_SI_UL) ? ul_cir_cbs :
												     dl_cir_cbs;
		result = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_METER, meter_idx, cfg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to cfg shared meter");
			doca_flow_destroy();
			return result;
		}

		ids_array[i] = meter_idx;
	}

	result = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_METER, ids_array, pdr->qerids_num, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to bind shared meters to port");
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Init shared meters level - one for each port and for each domain
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @pdr [in]: UPF PDR
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_shared_meters_level_init(struct upf_accel_ctx *upf_accel_ctx,
						       const struct upf_accel_pdr *pdr)
{
	struct doca_flow_shared_resource_cfg cfg = {.meter_cfg = {.limit_type = DOCA_FLOW_METER_LIMIT_TYPE_BYTES,
								  .color_mode = DOCA_FLOW_METER_COLOR_MODE_BLIND,
								  .alg = DOCA_FLOW_METER_ALGORITHM_TYPE_RFC2697,
								  .rfc2697.ebs = 0}};
	enum upf_accel_port port_id;
	doca_error_t result;

	for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
		result = upf_accel_shared_meters_dev_init(upf_accel_ctx, &cfg, pdr, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to init DOCA shared meters port %u tx: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Init all shared meters level
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_shared_meters_init(struct upf_accel_ctx *upf_accel_ctx)
{
	struct upf_accel_pdrs *pdrs = upf_accel_ctx->upf_accel_cfg->pdrs;
	const struct upf_accel_pdr *pdr;
	doca_error_t result;
	uint32_t pdr_idx;

	for (pdr_idx = 0; pdr_idx < pdrs->num_pdrs; ++pdr_idx) {
		pdr = &pdrs->arr_pdrs[pdr_idx];

		result = upf_accel_shared_meters_level_init(upf_accel_ctx, pdr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to init DOCA shared meters of pdr %u: %s",
				     pdr_idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Insert a static PDR entry
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @cfg [in]: entry configuration.
 * @entry [out]: Resulting entry.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pipe_pdr_insert(struct upf_accel_ctx *upf_accel_ctx,
				    struct upf_accel_entry_cfg *cfg,
				    struct doca_flow_pipe_entry **entry)
{
	const doca_error_t result = upf_accel_pipe_static_entry_add(upf_accel_ctx,
								    cfg->port_id,
								    0,
								    cfg->pipe,
								    cfg->match,
								    cfg->action,
								    cfg->mon,
								    cfg->fwd,
								    0,
								    &upf_accel_ctx->static_entry_ctx[cfg->port_id],
								    entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add pdr entry: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

/*
 * Get far struct that has ID matching to far_id
 *
 * @fars [in]: A struct containing all the fars pf the app.
 * @far_id [in]: FAR ID.
 * @return: A pointer to the far with far_id on success and NULL otherwise.
 */
static inline const struct upf_accel_far *upf_accel_get_far_by_id(const struct upf_accel_fars *fars, uint32_t far_id)
{
	for (uint32_t i = 0; i < fars->num_fars; ++i) {
		if (fars->arr_fars[i].id == far_id)
			return &fars->arr_fars[i];
	}

	return NULL;
}

/*
 * Insert entry to the encap & counter pipe
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @pdr_idx [in]: Location of PDR in the PDRs array.
 * @pdr_id [in]: PDR ID.
 * @far_id [in]: FAR ID.
 * @qfi [in]: QFI ID or 0 if doesn't exist.
 * @port_id [in]: port ID.
 * @entry [out]: Resulting entry.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_pipe_encap_counter_insert(struct upf_accel_ctx *upf_accel_ctx,
							uint32_t pdr_idx,
							uint32_t pdr_id,
							uint32_t far_id,
							uint8_t qfi,
							enum upf_accel_pdr_pdi_si pdi_si,
							enum upf_accel_port port_id,
							struct doca_flow_pipe_entry **entry)
{
	const struct upf_accel_far *far = upf_accel_get_far_by_id(upf_accel_ctx->upf_accel_cfg->fars, far_id);
	struct doca_flow_actions act_enc = {.encap_cfg.encap.tun.gtp_ext_psc_qfi = qfi & 0x3f};
	struct doca_flow_actions act_none = {.action_idx = UPF_ACCEL_ENCAP_ACTION_NONE};
	struct doca_flow_monitor mon = {
		.shared_counter = {.shared_counter_id = port_id_and_idx_to_quota_counter(port_id, pdr_idx)}};
	struct doca_flow_match match = {.meta.pkt_meta = DOCA_HTOBE32(pdr_id)};
	struct upf_accel_entry_cfg entry_cfg = {.match = &match,
						.fwd = NULL,
						.domain = DOCA_FLOW_PIPE_DOMAIN_EGRESS,
						.entry_idx = pdr_id,
						.port_id = port_id,
						.mon = &mon,
						.pipe = upf_accel_ctx->pipes[port_id][UPF_ACCEL_PIPE_TX_COUNTER]};

	if (far == NULL) {
		DOCA_LOG_ERR("Couldn't find FAR ID");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (far->fp_oh_ip.ip_version == DOCA_FLOW_L3_TYPE_IP4 && !!qfi) { /* Encap with IPv4 5G */
		act_enc.action_idx = UPF_ACCEL_ENCAP_ACTION_IPV4_5G;
		act_enc.encap_cfg.encap.outer.ip4.dst_ip = DOCA_HTOBE32(far->fp_oh_ip.addr.v4);
	} else if (far->fp_oh_ip.ip_version == DOCA_FLOW_L3_TYPE_IP6 && !!qfi) { /* Encap with IPv6 5G */
		act_enc.action_idx = UPF_ACCEL_ENCAP_ACTION_IPV6_5G;
		memcpy(act_enc.encap_cfg.encap.outer.ip6.dst_ip, far->fp_oh_ip.addr.v6, UPF_ACCEL_NUM_BYTES_IPV6);
	} else if (far->fp_oh_ip.ip_version == DOCA_FLOW_L3_TYPE_IP4 && !qfi) { /* Encap with IPv4 4G */
		act_enc.action_idx = UPF_ACCEL_ENCAP_ACTION_IPV4_4G;
		act_enc.encap_cfg.encap.outer.ip4.dst_ip = DOCA_HTOBE32(far->fp_oh_ip.addr.v4);
	} else if (far->fp_oh_ip.ip_version == DOCA_FLOW_L3_TYPE_IP6 && !qfi) { /* Encap with IPv6 4G */
		act_enc.action_idx = UPF_ACCEL_ENCAP_ACTION_IPV6_4G;
		memcpy(act_enc.encap_cfg.encap.outer.ip6.dst_ip, far->fp_oh_ip.addr.v6, UPF_ACCEL_NUM_BYTES_IPV6);
	}

	entry_cfg.action = (pdi_si == UPF_ACCEL_PDR_PDI_SI_DL) ? &act_enc : &act_none;

	act_enc.encap_cfg.encap.tun.gtp_teid = DOCA_HTOBE32(far->fp_oh_teid);

	if (pipe_pdr_insert(upf_accel_ctx, &entry_cfg, entry)) {
		DOCA_LOG_ERR("Failed to insert pdr entry: %u", DOCA_BETOH32(entry_cfg.match->meta.pkt_meta));
		return -1;
	}

	return DOCA_SUCCESS;
}

/*
 * Insert entry to the both directions counter pipes
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @pdr_idx [in]: Location of PDR in the PDRs array.
 * @pdr_id [in]: PDR ID.
 * @far_id [in]: FAR ID.
 * @qfi [in]: QFI ID or 0 if doesn't exist.
 * @entry [out]: pipe entries.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_tx_counters_insert(struct upf_accel_ctx *upf_accel_ctx,
						 uint32_t pdr_idx,
						 uint32_t pdr_id,
						 uint32_t far_id,
						 uint8_t qfi,
						 enum upf_accel_pdr_pdi_si pdi_si,
						 struct doca_flow_pipe_entry **entry)
{
	enum upf_accel_port port_id;

	for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
		if (upf_accel_pipe_encap_counter_insert(upf_accel_ctx,
							pdr_idx,
							pdr_id,
							far_id,
							qfi,
							pdi_si,
							port_id,
							&entry[port_id])) {
			DOCA_LOG_ERR("Failed to insert encap entry in port %u, pdr_id: %u", port_id, pdr_id);
			return -1;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Insert entry to the shared meters pipe
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @pdr_idx [in]: index of PDR in the PDRs array.
 * @qer_idx [in]: index of QER in the PDR's QERs array.
 * @port_id [in]: port ID .
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pipe_shared_meter_common_insert(struct upf_accel_ctx *upf_accel_ctx,
						    uint32_t pdr_idx,
						    uint32_t qer_idx,
						    enum upf_accel_port port_id)
{
	const struct upf_accel_pdrs *pdrs = upf_accel_ctx->upf_accel_cfg->pdrs;
	const struct upf_accel_pdr *pdr = &pdrs->arr_pdrs[pdr_idx];
	struct doca_flow_match match = {.meta.pkt_meta = DOCA_HTOBE32(pdr->id)};
	struct doca_flow_monitor mon = {
		.meter_type = DOCA_FLOW_RESOURCE_TYPE_SHARED,
	};
	struct doca_flow_fwd fwd = {
		.type = DOCA_FLOW_FWD_PIPE,
		.next_pipe = qer_idx == (pdr->qerids_num - 1) ?
				     upf_accel_ctx->pipes[port_id][UPF_ACCEL_PIPE_TX_COLOR_MATCH_NO_MORE_METERS] :
				     upf_accel_ctx->pipes[port_id][UPF_ACCEL_PIPE_TX_COLOR_MATCH_START + qer_idx]};
	struct upf_accel_entry_cfg entry_cfg = {
		.domain = DOCA_FLOW_PIPE_DOMAIN_EGRESS,
		.pipe = upf_accel_ctx->pipes[port_id][UPF_ACCEL_PIPE_TX_SHARED_METERS_START + qer_idx],
		.match = &match,
		.fwd = &fwd,
		.action = NULL,
		.mon = &mon,
		.entry_idx = pdr->id,
		.port_id = port_id};

	mon.shared_meter.shared_meter_id = upf_accel_shared_meters_table_offset_get(port_id, pdr_idx, qer_idx);

	if (pipe_pdr_insert(upf_accel_ctx, &entry_cfg, NULL)) {
		DOCA_LOG_ERR("Failed to insert p%d tx meter %u entry: %u", port_id, qer_idx, pdr->id);
		return -1;
	}

	return DOCA_SUCCESS;
}

/*
 * Insert entry to the both directions shared meters pipes
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @pdr_idx [in]: index of PDR in the PDRs array.
 * @qer_idx [in]: index of QER in the PDR's QERs array.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pipe_shared_meter_insert(struct upf_accel_ctx *upf_accel_ctx, uint32_t pdr_idx, uint32_t qer_idx)
{
	enum upf_accel_port port_id;
	doca_error_t result;

	for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
		result = pipe_shared_meter_common_insert(upf_accel_ctx, pdr_idx, qer_idx, port_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to insert PDR rule to port %u meter tables: %s",
				     port_id,
				     doca_error_get_descr(result));
			return result;
		}
	}
	return DOCA_SUCCESS;
}

/*
 * Add all SMF related rules
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_smf_rules_add(struct upf_accel_ctx *upf_accel_ctx)
{
	const struct upf_accel_pdrs *pdrs = upf_accel_ctx->upf_accel_cfg->pdrs;
	const size_t num_pdrs = pdrs->num_pdrs;
	const struct upf_accel_pdr *pdr;
	struct upf_accel_qer *qer;
	doca_error_t result;
	uint32_t pdr_idx;
	uint32_t far_id;
	uint8_t qfi;
	int pdr_id;
	uint32_t i;

	for (pdr_idx = 0; pdr_idx < num_pdrs; pdr_idx++) {
		pdr = &pdrs->arr_pdrs[pdr_idx];
		far_id = pdr->farid;
		pdr_id = pdr->id;
		qfi = UPF_ACCEL_QFI_NONE;

		if (pdr->qerids_num) {
			/* QFI is chosen randomly since different QERs might have different QFI values */
			qer = upf_accel_get_qer_by_qer_id(upf_accel_ctx->upf_accel_cfg->qers,
							  pdr->qerids[pdr->qerids_num - 1]);
			qfi = qer->qfi;
		}

		result = upf_accel_tx_counters_insert(upf_accel_ctx,
						      pdr_idx,
						      pdr_id,
						      far_id,
						      qfi,
						      pdr->pdi_si,
						      upf_accel_ctx->smf_entries[pdr_idx]);
		if (result != DOCA_SUCCESS)
			return result;

		for (i = 0; i < pdr->qerids_num; ++i) {
			result = pipe_shared_meter_insert(upf_accel_ctx, pdr_idx, i);
			if (result != DOCA_SUCCESS)
				return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Alloc and populate the counter IDs used for quota enforcement
 *
 * The indexes of the counters are organized as followed:
 * For PDR with ID i:
 *	port 0 counter index is i
 *	port 1 counter index is <num_of_shared_counters_per_port> + i
 *
 * @start_idx [in]: first pdr/index to start with
 * @num_ports [in]: number of ports
 * @num_cntrs [in]: number of counters
 * @shared_counter_ids [in/out]: pointer to store the result at
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_NO_MEMORY otherwise
 */
static doca_error_t alloc_and_populate_quota_counters_ids(uint16_t start_idx,
							  size_t num_ports,
							  size_t num_cntrs,
							  struct app_shared_counter_ids *shared_counter_ids)
{
	enum upf_accel_port port_id;
	uint32_t i;

	shared_counter_ids->cntr_0 = start_idx;
	shared_counter_ids->cntrs_num = num_cntrs;

	if (!num_cntrs)
		return DOCA_SUCCESS;

	for (port_id = 0; port_id < num_ports; port_id++) {
		shared_counter_ids->ids[port_id] =
			rte_calloc("Counter IDS", num_cntrs, sizeof(uint32_t), RTE_CACHE_LINE_SIZE);
		if (!shared_counter_ids->ids[port_id]) {
			DOCA_LOG_ERR("Failed to allocate Counter IDS array for port %u", port_id);
			goto cleanup;
		}

		for (i = 0; i < num_cntrs; ++i) {
			shared_counter_ids->ids[port_id][i] = port_id_and_idx_to_quota_counter(port_id, start_idx + i);
		}
	}

	return DOCA_SUCCESS;

cleanup:
	while (port_id--)
		rte_free(shared_counter_ids->ids[port_id]);
	return DOCA_ERROR_NO_MEMORY;
}

/*
 * Free the counter IDs arrays
 *
 * @shared_counter_ids [in]: Container of IDs to be released
 * @num_ports [in]: number of ports
 */
static void free_quota_counters_ids(struct app_shared_counter_ids *shared_counter_ids, uint16_t num_ports)
{
	enum upf_accel_port port_id;

	for (port_id = 0; port_id < num_ports; port_id++)
		rte_free(shared_counter_ids->ids[port_id]);
}

/*
 * Calculate Hash Table size per core
 *
 * The total amount of connections supported on each pipe is defined by
 * UPF_ACCEL_MAX_NUM_CONNECTIONS,
 * On each port we have 2 pipes (WAN: 5t_udp + 5t_tcp, RAN: 7t + 8t) so the
 * maximum connections per port is 2 * UPF_ACCEL_MAX_NUM_CONNECTIONS.
 * Then the result is distributed between the cores.
 * In addition, to each core some entries are added for failed acceleration connections.
 *
 * @num_cores [in]: number of cores
 * @return: hash table size per core
 */
static uint32_t calculate_hash_table_size(uint16_t num_cores)
{
	uint32_t max_conn_per_port = 2 * UPF_ACCEL_MAX_NUM_CONNECTIONS;
	uint32_t num_conn_per_core = (max_conn_per_port + num_cores - 1) / num_cores;

	return num_conn_per_core + UPF_ACCEL_MAX_NUM_FAILED_ACCEL_PER_CORE;
};

/*
 * Initiate the counters used for quota enforcement
 *
 * Quota enforcement counters defined by QER which we consume from
 * the PDR description (if exists), hence, the number of counters we'll have
 * will be at most as the number of PDRs, but per port (i.e. twice).
 *
 * @upf_accel_ctx [in]: UPF Acceleration context.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_init_quota_counters(struct upf_accel_ctx *upf_accel_ctx)
{
	struct doca_flow_shared_resource_cfg cfg = {0};
	struct app_shared_counter_ids shared_counter_ids;
	enum upf_accel_port port_id;
	doca_error_t result;
	uint32_t i;

	result = alloc_and_populate_quota_counters_ids(0,
						       upf_accel_ctx->num_ports,
						       upf_accel_ctx->upf_accel_cfg->pdrs->num_pdrs,
						       &shared_counter_ids);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to populate quota counters ids");
		return result;
	}

	for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
		for (i = 0; i < upf_accel_ctx->upf_accel_cfg->pdrs->num_pdrs; ++i) {
			result = doca_flow_shared_resource_set_cfg(DOCA_FLOW_SHARED_RESOURCE_COUNTER,
								   shared_counter_ids.ids[port_id][i],
								   &cfg);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to configure shared counter %u to port %d", i, port_id);
				goto cleanup;
			}
		}
		result = doca_flow_shared_resources_bind(DOCA_FLOW_SHARED_RESOURCE_COUNTER,
							 shared_counter_ids.ids[port_id],
							 upf_accel_ctx->upf_accel_cfg->pdrs->num_pdrs,
							 upf_accel_ctx->ports[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to bind shared counter to port %d", port_id);
			goto cleanup;
		}
	}

cleanup:
	free_quota_counters_ids(&shared_counter_ids, upf_accel_ctx->num_ports);
	return result;
}
/*
 * Cleanup and free UPF Acceleration flow processing data
 *
 * @fp_data_arr [in]: flow processing data array
 */
static void upf_accel_fp_data_cleanup(struct upf_accel_fp_data *fp_data_arr)
{
	struct upf_accel_fp_data *fp_data;
	unsigned int lcore;

	RTE_LCORE_FOREACH_WORKER(lcore)
	{
		fp_data = &fp_data_arr[lcore];

		free_quota_counters_ids(&fp_data->quota_cntrs, fp_data->ctx->num_ports);
		rte_free(fp_data->dyn_ext_tbl_data);
		rte_hash_free(fp_data->dyn_ext_tbl);
		rte_free(fp_data->dyn_tbl_data);
		rte_hash_free(fp_data->dyn_tbl);
	}

	rte_free(fp_data_arr);
}

/*
 * Allocate and initialize UPF Acceleration flow processing data
 *
 * @ctx [in]: UPF Acceleration context
 * @fp_data_arr_out [out]: flow processing data array
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_fp_data_init(struct upf_accel_ctx *ctx, struct upf_accel_fp_data **fp_data_arr_out)
{
	uint16_t num_cores = rte_lcore_count() - 1;

	assert(num_cores > 0);
	uint16_t quota_cntrs_per_core_num = ctx->upf_accel_cfg->pdrs->num_pdrs / num_cores;
	uint16_t quota_cntrs_remainder_num = ctx->upf_accel_cfg->pdrs->num_pdrs % num_cores;
	uint32_t ht_size = calculate_hash_table_size(num_cores);
	char mem_name[RTE_MEMZONE_NAMESIZE];
	struct rte_hash_parameters dyn_tbl_params = {
		.name = mem_name,
		.entries = ht_size,
		.key_len = sizeof(struct upf_accel_match_5t),
		.hash_func = rte_hash_crc,
		.hash_func_init_val = 0,
		.socket_id = rte_socket_id(),
		.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE,
	};
	struct rte_hash_parameters dyn_ext_tbl_params = {
		.name = mem_name,
		.entries = ht_size * PARSER_PKT_TYPE_NUM,
		.key_len = sizeof(struct upf_accel_match_extension),
		.hash_func = rte_hash_crc,
		.hash_func_init_val = 0,
		.socket_id = rte_socket_id(),
		.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE,
	};
	uint16_t curr_quota_base_cntr_idx = 0;
	struct upf_accel_fp_data *fp_data_arr;
	struct upf_accel_fp_data *fp_data;
	uint16_t queue_id = 1;
	unsigned int lcore;
	size_t num_cntrs;
	doca_error_t res;
	uint32_t i;

	fp_data_arr = rte_calloc("FP data", RTE_MAX_LCORE, sizeof(*fp_data), RTE_CACHE_LINE_SIZE);
	if (!fp_data_arr) {
		DOCA_LOG_ERR("Failed to allocate FP data");
		return DOCA_ERROR_NO_MEMORY;
	}

	DOCA_LOG_DBG("Main core is %u", rte_get_main_lcore());

	RTE_LCORE_FOREACH_WORKER(lcore)
	{
		fp_data = &fp_data_arr[lcore];

		snprintf(mem_name, sizeof(mem_name), "Dyn conn ht %u", lcore);
		fp_data->dyn_tbl = rte_hash_create(&dyn_tbl_params);
		if (!fp_data->dyn_tbl) {
			DOCA_LOG_ERR("Failed to allocate dynamic connection table");
			res = DOCA_ERROR_NO_MEMORY;
			goto cleanup;
		}

		snprintf(mem_name, sizeof(mem_name), "Dyn conn data %u", lcore);
		fp_data->dyn_tbl_data = rte_calloc(mem_name,
						   dyn_tbl_params.entries,
						   sizeof(*fp_data->dyn_tbl_data),
						   RTE_CACHE_LINE_SIZE);
		if (!fp_data->dyn_tbl_data) {
			DOCA_LOG_ERR("Failed to allocate dynamic connection table data");
			res = DOCA_ERROR_NO_MEMORY;
			goto cleanup;
		}

		snprintf(mem_name, sizeof(mem_name), "Dyn ext ht %u", lcore);
		fp_data->dyn_ext_tbl = rte_hash_create(&dyn_ext_tbl_params);
		if (!fp_data->dyn_ext_tbl) {
			DOCA_LOG_ERR("Failed to allocate dynamic extension table");
			res = DOCA_ERROR_NO_MEMORY;
			goto cleanup;
		}

		snprintf(mem_name, sizeof(mem_name), "Dyn ext data %u", lcore);
		fp_data->dyn_ext_tbl_data = rte_calloc(mem_name,
						       dyn_ext_tbl_params.entries,
						       sizeof(*fp_data->dyn_ext_tbl_data),
						       RTE_CACHE_LINE_SIZE);
		if (!fp_data->dyn_ext_tbl_data) {
			DOCA_LOG_ERR("Failed to allocate dynamic extension table data");
			res = DOCA_ERROR_NO_MEMORY;
			goto cleanup;
		}

		for (i = 0; i < dyn_ext_tbl_params.entries; ++i) {
			fp_data->dyn_ext_tbl_data[i].type = UPF_ACCEL_RULE_DYNAMIC_EXTENSION;
		}

		num_cntrs = quota_cntrs_per_core_num;
		/* The remainder is distributed among the first cores so that it is almost evenly distributed */
		if (quota_cntrs_remainder_num) {
			num_cntrs++;
			quota_cntrs_remainder_num--;
		}

		res = alloc_and_populate_quota_counters_ids(curr_quota_base_cntr_idx,
							    ctx->num_ports,
							    num_cntrs,
							    &fp_data->quota_cntrs);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to populate quota counters ids for core %u", lcore);
			goto cleanup;
		}
		curr_quota_base_cntr_idx += num_cntrs;

		fp_data->ctx = ctx;
		fp_data->queue_id = queue_id++;

		upf_accel_sw_aging_ll_init(fp_data, PARSER_PKT_TYPE_TUNNELED);
		upf_accel_sw_aging_ll_init(fp_data, PARSER_PKT_TYPE_PLAIN);

		DOCA_LOG_DBG("FP core %u data initialized", lcore);
	}

	*fp_data_arr_out = fp_data_arr;
	return DOCA_SUCCESS;

cleanup:
	upf_accel_fp_data_cleanup(fp_data_arr);
	return res;
}

/*
 * Fill DOCA device array to use during port starting.
 *
 * @ctx [in] UPF Acceleration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t fill_device_array(struct upf_accel_ctx *ctx)
{
	doca_error_t ret;
	int port_id;

	for (port_id = 0; port_id < ctx->num_ports; port_id++) {
		ret = doca_dpdk_port_as_dev(port_id, &ctx->dev_arr[port_id]);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get device for port %d: %s", port_id, doca_error_get_descr(ret));
			return ret;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Print FP SW debug counters of each worker
 *
 * @fp_data_arr [in]: flow processing data array
 */
static void upf_accel_fp_sw_counters_print(struct upf_accel_fp_data *fp_data_arr)
{
	struct upf_accel_fp_sw_counters sw_sum = {0};
	struct upf_accel_fp_sw_counters *sw_counters;
	struct upf_accel_fp_data *fp_data;
	unsigned int lcore;

	DOCA_LOG_INFO("//////////////////// SW DATAPATH COUNTERS ////////////////////");

	RTE_LCORE_FOREACH_WORKER(lcore)
	{
		fp_data = &fp_data_arr[lcore];
		sw_counters = &fp_data->sw_counters;

		DOCA_LOG_INFO(
			"Core %3u sw new_pkts=%-8lu new_bytes=%-8lu ex_pkts=%-8lu ex_bytes=%-8lu err_pkts=%-8lu err_bytes=%-8lu",
			lcore,
			sw_counters->new_conn.pkts,
			sw_counters->new_conn.bytes,
			sw_counters->ex_conn.pkts,
			sw_counters->ex_conn.bytes,
			sw_counters->err.pkts,
			sw_counters->err.bytes);

		sw_sum.new_conn.pkts += sw_counters->new_conn.pkts;
		sw_sum.new_conn.bytes += sw_counters->new_conn.bytes;
		sw_sum.ex_conn.pkts += sw_counters->ex_conn.pkts;
		sw_sum.ex_conn.bytes += sw_counters->ex_conn.bytes;
		sw_sum.err.pkts += sw_counters->err.pkts;
		sw_sum.err.bytes += sw_counters->err.bytes;
	}

	DOCA_LOG_INFO(
		"TOTAL sw    new_pkts=%-8lu new_bytes=%-8lu ex_pkts=%-8lu ex_bytes=%-8lu err_pkts=%-8lu err_bytes=%-8lu",
		sw_sum.new_conn.pkts,
		sw_sum.new_conn.bytes,
		sw_sum.ex_conn.pkts,
		sw_sum.ex_conn.bytes,
		sw_sum.err.pkts,
		sw_sum.err.bytes);
}

/*
 * Print FP HW acceleration debug counters of each worker
 *
 * @fp_data_arr [in]: flow processing data array
 * @name [in]: name of the acceleration counters
 * @failed [in]: A flag for printing the successful acceleration counters
 * (false) or those of the failed (true)
 */
static void upf_accel_fp_accel_counters_print(struct upf_accel_fp_data *fp_data_arr, const char *name)
{
	struct upf_accel_fp_accel_counters *ran_counters;
	struct upf_accel_fp_accel_counters *wan_counters;
	struct upf_accel_fp_accel_counters ran_sum = {0};
	struct upf_accel_fp_accel_counters wan_sum = {0};
	struct upf_accel_fp_data *fp_data;
	unsigned int lcore;

	DOCA_LOG_INFO("//////////////////// %s COUNTERS ////////////////////", name);

	RTE_LCORE_FOREACH_WORKER(lcore)
	{
		fp_data = &fp_data_arr[lcore];
		if (strcmp(name, "ACCELERATED") == 0) {
			ran_counters = &fp_data->accel_counters[PARSER_PKT_TYPE_TUNNELED];
			wan_counters = &fp_data->accel_counters[PARSER_PKT_TYPE_PLAIN];
		} else if (strcmp(name, "NOT ACCELERATED") == 0) {
			ran_counters = &fp_data->unaccel_counters[PARSER_PKT_TYPE_TUNNELED];
			wan_counters = &fp_data->unaccel_counters[PARSER_PKT_TYPE_PLAIN];
		} else if (strcmp(name, "ACCELERATION FAILED") == 0) {
			ran_counters = &fp_data->accel_failed_counters[PARSER_PKT_TYPE_TUNNELED];
			wan_counters = &fp_data->accel_failed_counters[PARSER_PKT_TYPE_PLAIN];
		} else {
			DOCA_LOG_ERR("Unknowon counters name");
			return;
		}

		DOCA_LOG_INFO(
			"Core %3u %s ran_current=%-8lu ran_aged=%-8lu ran_total=%-8lu ran_accel_errors=%-8lu ran_aging_errors=%-8lu ran_del_errors=%-8lu wan_current=%-8lu wan_aged=%-8lu wan_total=%-8lu wan_accel_errors=%-8lu wan_aging_errors=%-8lu wan_del_errors=%-8lu",
			lcore,
			name,
			ran_counters->current,
			ran_counters->total - ran_counters->current,
			ran_counters->total,
			ran_counters->accel_errors,
			ran_counters->aging_errors,
			ran_counters->del_errors,
			wan_counters->current,
			wan_counters->total - wan_counters->current,
			wan_counters->total,
			wan_counters->accel_errors,
			wan_counters->aging_errors,
			wan_counters->del_errors);

		ran_sum.current += ran_counters->current;
		ran_sum.total += ran_counters->total;
		ran_sum.accel_errors += ran_counters->accel_errors;
		ran_sum.aging_errors += ran_counters->aging_errors;
		ran_sum.del_errors += ran_counters->del_errors;
		wan_sum.current += wan_counters->current;
		wan_sum.total += wan_counters->total;
		wan_sum.accel_errors += wan_counters->accel_errors;
		wan_sum.aging_errors += wan_counters->aging_errors;
		wan_sum.del_errors += wan_counters->del_errors;
	}

	DOCA_LOG_INFO(
		"TOTAL    %s ran_current=%-8lu ran_aged=%-8lu ran_total=%-8lu ran_accel_errors=%-8lu ran_aging_errors=%-8lu ran_del_errors=%-8lu wan_current=%-8lu wan_aged=%-8lu wan_total=%-8lu wan_accel_errors=%-8lu wan_aging_errors=%-8lu wan_del_errors=%-8lu",
		name,
		ran_sum.current,
		ran_sum.total - ran_sum.current,
		ran_sum.total,
		ran_sum.accel_errors,
		ran_sum.aging_errors,
		ran_sum.del_errors,
		wan_sum.current,
		wan_sum.total - wan_sum.current,
		wan_sum.total,
		wan_sum.accel_errors,
		wan_sum.aging_errors,
		wan_sum.del_errors);
}

/*
 * Print PDR counters of each worker
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 */
static void upf_accel_pdrs_print(struct upf_accel_ctx *upf_accel_ctx)
{
	const struct upf_accel_pdrs *pdrs = upf_accel_ctx->upf_accel_cfg->pdrs;
	struct doca_flow_resource_query pdr_sum = {0};
	struct doca_flow_resource_query pdr_stats;
	const struct upf_accel_pdr *pdr;
	enum upf_accel_port port_id;
	doca_error_t result;
	uint32_t pdr_idx;
	int pdr_id;

	DOCA_LOG_INFO("//////////////////// HW PDR COUNTERS ////////////////////");

	for (pdr_idx = 0; pdr_idx < pdrs->num_pdrs; pdr_idx++) {
		pdr = &pdrs->arr_pdrs[pdr_idx];
		pdr_id = pdr->id;

		pdr_sum.counter.total_pkts = 0;
		pdr_sum.counter.total_bytes = 0;

		for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
			result = doca_flow_resource_query_entry(upf_accel_ctx->smf_entries[pdr_idx][port_id],
								&pdr_stats);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Error querying port %u PDR counter %d: %s",
					     port_id,
					     pdr_id,
					     doca_error_get_descr(result));
				continue;
			}

			DOCA_LOG_INFO("PDR %2d Port %d pkts=%-8lu bytes=%-8lu",
				      pdr_id,
				      port_id,
				      pdr_stats.counter.total_pkts,
				      pdr_stats.counter.total_bytes);

			pdr_sum.counter.total_pkts += pdr_stats.counter.total_pkts;
			pdr_sum.counter.total_bytes += pdr_stats.counter.total_bytes;
		}

		DOCA_LOG_INFO("PDR %2d TOTAL  pkts=%-8lu bytes=%-8lu",
			      pdr_id,
			      pdr_sum.counter.total_pkts,
			      pdr_sum.counter.total_bytes);
	}
}

/*
 * Print Drop Counter
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @drop_type [in]: Drop pipe type
 */
static void upf_accel_drop_counter_print(struct upf_accel_ctx *upf_accel_ctx, enum upf_accel_pipe_drop_type drop_type)
{
	struct doca_flow_resource_query rx_dropped_stats, rx_dropped_stats_sum = {0};
	struct doca_flow_resource_query tx_dropped_stats, tx_dropped_stats_sum = {0};
	struct doca_flow_pipe_entry **entries = upf_accel_ctx->drop_entries[drop_type];
	enum upf_accel_port port_id;
	doca_error_t result;
	uint8_t entry_idx;
	char *name;

	switch (drop_type) {
	case UPF_ACCEL_DROP_DBG:
		name = "Debug";
		break;
	case UPF_ACCEL_DROP_RATE:
		name = "Rate";
		break;
	case UPF_ACCEL_DROP_FILTER:
		name = "Filter";
		break;
	default:
		assert(0);
		name = "Error!";
	}

	for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
		entry_idx = upf_accel_domain_idx_get(port_id, DOCA_FLOW_PIPE_DOMAIN_DEFAULT);
		result = doca_flow_resource_query_entry(entries[entry_idx], &rx_dropped_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Error querying port %u RX %s Drop counter: %s",
				     port_id,
				     name,
				     doca_error_get_descr(result));
			return;
		}
		entry_idx = upf_accel_domain_idx_get(port_id, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
		result = doca_flow_resource_query_entry(entries[entry_idx], &tx_dropped_stats);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Error querying port %u TX %s Drop counter: %s",
				     port_id,
				     name,
				     doca_error_get_descr(result));
			return;
		}

		DOCA_LOG_INFO("Port %d DROP %-8s rx_pkts=%-8lu tx_pkts=%-8lu",
			      port_id,
			      name,
			      rx_dropped_stats.counter.total_pkts,
			      tx_dropped_stats.counter.total_pkts);

		rx_dropped_stats_sum.counter.total_pkts += rx_dropped_stats.counter.total_pkts;
		tx_dropped_stats_sum.counter.total_pkts += tx_dropped_stats.counter.total_pkts;
	}

	DOCA_LOG_INFO("TOTAL  DROP %-8s rx_pkts=%-8lu tx_pkts=%-8lu",
		      name,
		      rx_dropped_stats_sum.counter.total_pkts,
		      tx_dropped_stats_sum.counter.total_pkts);
}

/*
 * Print Counters of The Static HW entries
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 */
static void upf_accel_static_hw_counters_print(struct upf_accel_ctx *upf_accel_ctx)
{
	int i;

	DOCA_LOG_INFO("//////////////////// STATIC HW COUNTERS ////////////////////");

	for (i = 0; i < UPF_ACCEL_DROP_NUM; i++)
		upf_accel_drop_counter_print(upf_accel_ctx, i);
}

/*
 * Print FP debug counters of each worker
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @fp_data_arr [in]: flow processing data array
 */
static void upf_accel_debug_counters_print(struct upf_accel_ctx *upf_accel_ctx, struct upf_accel_fp_data *fp_data_arr)
{
	DOCA_LOG_INFO("");
	upf_accel_fp_sw_counters_print(fp_data_arr);
	DOCA_LOG_INFO("");
	upf_accel_fp_accel_counters_print(fp_data_arr, "ACCELERATED");
	DOCA_LOG_INFO("");
	upf_accel_fp_accel_counters_print(fp_data_arr, "NOT ACCELERATED");
	DOCA_LOG_INFO("");
	upf_accel_fp_accel_counters_print(fp_data_arr, "ACCELERATION FAILED");
	DOCA_LOG_INFO("");
	upf_accel_pdrs_print(upf_accel_ctx);
	DOCA_LOG_INFO("");
	upf_accel_static_hw_counters_print(upf_accel_ctx);
	DOCA_LOG_INFO("");
}

/*
 * Mask signals that will be handled by the application main loop
 *
 * @sigset [out]: Set of masked signals
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_signals_mask(sigset_t *sigset)
{
	int ret;

	sigemptyset(sigset);
	sigaddset(sigset, SIGINT);
	sigaddset(sigset, SIGTERM);
	sigaddset(sigset, SIGUSR1);

	ret = pthread_sigmask(SIG_BLOCK, sigset, NULL);
	if (ret) {
		DOCA_LOG_ERR("Failed to apply signal mask: %s", strerror(ret));
		return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

/*
 * FP thread main run loop
 *
 * @param [in]: Array of FP data structures
 * @return: 0 on success and system error code otherwise
 */
static int upf_accel_fp_loop_wrapper(void *param)
{
	struct upf_accel_fp_data *fp_data_arr = param;

	upf_accel_fp_loop(&fp_data_arr[rte_lcore_id()]);
	return 0;
}

/*
 * Calculate the maximum number of shared meters needed
 *
 * @num_ports [in]: number of ports
 * @return: number of shared meters
 */
static inline int upf_accel_calc_num_shared_meters(uint16_t num_ports)
{
	return upf_accel_shared_meters_table_offset_get(num_ports,
							UPF_ACCEL_MAX_NUM_PDR,
							UPF_ACCEL_MAX_PDR_NUM_RATE_METERS);
}

/*
 * Calculate the maximum number of shared counters needed
 *
 * @num_ports [in]: number of ports
 * @return: number of shared counters
 */
static inline int upf_accel_calc_num_shared_counters(uint16_t num_ports)
{
	return UPF_ACCEL_NUM_QUOTA_COUNTERS_PER_PORT * num_ports;
}

/*
 * Get the forwarding port in single port mode.
 * In single port mode packets are sent from the same port.
 *
 * @port_id [in]: port ID
 * @return: the forwarding port
 */
static enum upf_accel_port upf_accel_single_port_get_fwd_port(enum upf_accel_port port_id)
{
	return port_id;
}

/*
 * Initialize DOCA Flow
 *
 * @nb_queues [in]: number of queues
 * @mode [in]: HW running mode
 * @resource [in]: flow resources
 * @nr_shared_resources [in]: number of shared flow resources
 * @cb [in]: callback function for processing flow entries
 */
static doca_error_t upf_accel_doca_flow_init(int nb_queues,
					     const char *mode,
					     struct flow_resources *resource,
					     uint32_t nr_shared_resources[],
					     doca_flow_entry_process_cb cb)
{
	struct doca_flow_cfg *flow_cfg;
	uint16_t qidx, rss_queues[nb_queues];
	struct doca_flow_resource_rss_cfg rss = {0};
	doca_error_t result, tmp_result;

	result = doca_flow_cfg_create(&flow_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	rss.nr_queues = nb_queues;
	for (qidx = 0; qidx < nb_queues; qidx++)
		rss_queues[qidx] = qidx;
	rss.queues_array = rss_queues;
	result = doca_flow_cfg_set_default_rss(flow_cfg, &rss);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg rss: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_pipe_queues(flow_cfg, nb_queues);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg pipe_queues: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_mode_args(flow_cfg, mode);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg mode_args: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_nr_counters(flow_cfg, resource->nr_counters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_counters: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_nr_meters(flow_cfg, resource->nr_meters);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_meters: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_cfg_set_cb_entry_process(flow_cfg, cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg doca_flow_entry_process_cb: %s",
			     doca_error_get_descr(result));
		goto destroy_cfg;
	}

	for (int i = 0; i < SHARED_RESOURCE_NUM_VALUES; i++) {
		result = doca_flow_cfg_set_nr_shared_resource(flow_cfg, nr_shared_resources[i], i);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_cfg nr_shared_resources: %s",
				     doca_error_get_descr(result));
			goto destroy_cfg;
		}
	}

	result = doca_flow_cfg_set_rss_key(flow_cfg, upf_accel_rss_hash_key, UPF_ACCEL_RSS_KEY_LEN);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_cfg rss key: %s", doca_error_get_descr(result));
		goto destroy_cfg;
	}

	result = doca_flow_init(flow_cfg);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to initialize DOCA Flow: %s", doca_error_get_descr(result));
destroy_cfg:
	tmp_result = doca_flow_cfg_destroy(flow_cfg);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy doca_flow_cfg: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

/*
 * UPF Acceleration application deinitialization
 *
 * @upf_accel_ctx [in/out]: UPF Acceleration context
 * @fp_data_arr [in]: UPF Acceleration flow processing data
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t deinit_upf_accel(struct upf_accel_ctx *upf_accel_ctx, struct upf_accel_fp_data *fp_data_arr)
{
	doca_error_t result;

	result = stop_doca_flow_ports(upf_accel_ctx->num_ports, upf_accel_ctx->ports);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop doca flow ports: %s", doca_error_get_descr(result));
	}

	upf_accel_fp_data_cleanup(fp_data_arr);
	doca_flow_destroy();

	return result;
}

/*
 * UPF Acceleration application initialization
 *
 * @upf_accel_ctx [in/out]: UPF Acceleration context
 * @fp_data_arr [out]: UPF Acceleration flow processing data
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_upf_accel(struct upf_accel_ctx *upf_accel_ctx, struct upf_accel_fp_data **fp_data_arr)
{
	uint32_t actions_mem_size[UPF_ACCEL_PORTS_MAX];
	struct entries_status *ctrl_status;
	doca_error_t result, tmp_result;
	enum upf_accel_port port_id;

	result = upf_accel_doca_flow_init(upf_accel_ctx->num_queues,
					  "vnf,hws",
					  &upf_accel_ctx->resource,
					  upf_accel_ctx->num_shared_resources,
					  upf_accel_check_for_valid_entry_aging);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	result = upf_accel_fp_data_init(upf_accel_ctx, fp_data_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init FP data");
		goto cleanup_doca_flow;
	}

	result = fill_device_array(upf_accel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill device array");
		goto cleanup_fp_data;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(UPF_ACCEL_MAX_NUM_CONNECTIONS));
	result = init_doca_flow_ports(upf_accel_ctx->num_ports,
				      upf_accel_ctx->ports,
				      true,
				      upf_accel_ctx->dev_arr,
				      actions_mem_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		goto cleanup_fp_data;
	}

	result = upf_accel_init_quota_counters(upf_accel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA shared counters: %s", doca_error_get_descr(result));
		goto cleanup_ports;
	}

	result = upf_accel_shared_meters_init(upf_accel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA shared meters: %s", doca_error_get_descr(result));
		goto cleanup_ports;
	}

	result = upf_accel_pipeline_create(upf_accel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipeline: %s", doca_error_get_descr(result));
		goto cleanup_ports;
	}

	if (upf_accel_smf_rules_add(upf_accel_ctx)) {
		DOCA_LOG_ERR("Failed to add smf rules");
		goto cleanup_ports;
	}

	for (port_id = 0; port_id < upf_accel_ctx->num_ports; port_id++) {
		ctrl_status = &upf_accel_ctx->static_entry_ctx[port_id].static_ctx.ctrl_status;

		result = doca_flow_entries_process(upf_accel_ctx->ports[port_id],
						   0,
						   DEFAULT_TIMEOUT_US,
						   upf_accel_ctx->num_static_entries[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process entries on port %u: %s", port_id, doca_error_get_descr(result));
			goto cleanup_ports;
		}

		if (ctrl_status->nb_processed != (int)upf_accel_ctx->num_static_entries[port_id] ||
		    ctrl_status->failure) {
			DOCA_LOG_ERR("Failed to process port %u entries", port_id);
			goto cleanup_ports;
		}
	}

	return DOCA_SUCCESS;

cleanup_ports:
	tmp_result = stop_doca_flow_ports(upf_accel_ctx->num_ports, upf_accel_ctx->ports);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop doca flow ports: %s", doca_error_get_descr(tmp_result));
	}
cleanup_fp_data:
	upf_accel_fp_data_cleanup(*fp_data_arr);
cleanup_doca_flow:
	doca_flow_destroy();

	return result;
}

/*
 * UPF Acceleration application logic
 *
 * @upf_accel_ctx [in]: UPF Acceleration context
 * @fp_data_arr [in]: UPF Acceleration flow processing data
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_upf_accel(struct upf_accel_ctx *upf_accel_ctx, struct upf_accel_fp_data *fp_data_arr)
{
	doca_error_t result;
	sigset_t sigset;
	int sig;
	int ret;

	result = upf_accel_signals_mask(&sigset);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to mask signals");
		return result;
	}

	if (rte_eal_mp_remote_launch(upf_accel_fp_loop_wrapper, fp_data_arr, SKIP_MAIN)) {
		DOCA_LOG_ERR("Failed to launch FP threads");
		return result;
	}

	DOCA_LOG_INFO("Waiting for traffic, press Ctrl+C for termination");

	while (!force_quit) {
		ret = sigwait(&sigset, &sig);
		if (ret) {
			DOCA_LOG_ERR("Failed to sigwait: %s", strerror(ret));
			return result;
		}

		switch (sig) {
		case SIGINT:
		case SIGTERM:
			force_quit = true;
			break;
		case SIGUSR1:
			upf_accel_debug_counters_print(upf_accel_ctx, fp_data_arr);
			break;
		default:
			DOCA_LOG_WARN("Polled unexpected signal %d", sig);
			break;
		}
	}

	rte_eal_mp_wait_lcore();

	upf_accel_debug_counters_print(upf_accel_ctx, fp_data_arr);

	return DOCA_SUCCESS;
}

/*
 * Callback to handle file smf json path
 *
 * @param [in]: file path
 * @config [in]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t smf_config_file_path_callback(void *param, void *config)
{
	struct upf_accel_config *cfg = (struct upf_accel_config *)config;
	const char *n = (const char *)param;

	cfg->smf_config_file_path = n;

	return DOCA_SUCCESS;
}

/*
 * Callback to handle file vxlan json path
 *
 * @param [in]: file path
 * @config [in]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vxlan_config_file_path_callback(void *param, void *config)
{
	struct upf_accel_config *cfg = (struct upf_accel_config *)config;
	const char *n = (const char *)param;

	cfg->vxlan_config_file_path = n;

	return DOCA_SUCCESS;
}

/*
 * Callback to handle file path
 *
 * @param [in]: file path
 * @config [in]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t aging_time_sec_callback(void *param, void *config)
{
	struct upf_accel_config *cfg = (struct upf_accel_config *)config;
	const uint32_t n = *(const uint32_t *)param;

	cfg->hw_aging_time_sec = n;
	cfg->sw_aging_time_sec = n;

	return DOCA_SUCCESS;
}

/*
 * Callback to handle number of pkts before acceleration param
 *
 * @param [in]: input param (num pkts)
 * @config [in]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pkts_before_accel_callback(void *param, void *config)
{
	struct upf_accel_config *cfg = (struct upf_accel_config *)config;
	const uint32_t n = *(const uint32_t *)param;

	if (n < 1) {
		DOCA_LOG_ERR("Bad param: num-pkts-pefore-accel must be greater or equal to 1");
		return DOCA_ERROR_INVALID_VALUE;
	}

	cfg->dpi_threshold = n;

	return DOCA_SUCCESS;
}

/*
 * Callback to handle UL port number in fixed port mode
 *
 * Fixed port mode is a configuration where each port receives only one type
 * of packet, either uplink (UL - GTP) or downlink (DL - non-GTP).
 * Using the '-p' flag we can activate this mode and specify which port will
 * receive UL packets, while the other port will receive DL packets.
 *
 * In this mode, the uldl_pipe on the designated UL port directs all packets
 * to the ext_gtp pipe (without matching on UDP dest port), while on the
 * DL port, all packets are directed to the 5t pipe.
 *
 * This mode is necessary for Trex tests, as packets sent by Trex cannot be
 * distinguished between GTP and non-GTP solely by examining the UDP dest port.
 *
 * @param [in]: input param (UL port number).
 * @config [in]: UPF Acceleration configuration.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t fixed_port_callback(void *param, void *config)
{
	struct upf_accel_config *cfg = (struct upf_accel_config *)config;
	const int32_t n = *(const int32_t *)param;

	if (n < 0 || n >= UPF_ACCEL_PORTS_MAX) {
		DOCA_LOG_ERR("Bad param: Invalid fixed-port number.");
		return DOCA_ERROR_INVALID_VALUE;
	}

	cfg->fixed_port = n;

	return DOCA_SUCCESS;
}

/*
 * Convert the user context to the flow_dev_ctx struct
 *
 * @user_ctx [in]: User context
 * @return: Flow device context
 */
static struct flow_dev_ctx *flow_dev_ctx_from_user_ctx(void *user_ctx)
{
	return &((struct upf_accel_config *)user_ctx)->flow_devs;
}

/*
 * Handle application parameters registration
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_register_params(void)
{
	struct doca_argp_param *pdr_file_path_param;
	struct doca_argp_param *vxlan_file_path_param;
	struct doca_argp_param *aging_time_sec_param;
	struct doca_argp_param *pkts_before_accel_param;
	struct doca_argp_param *fixed_port_param;
	doca_error_t result;

	/* Register flow device params */
	result = register_flow_device_params(flow_dev_ctx_from_user_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow device params: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register UPF Acceleration JSON PDR definitions file path */
	result = doca_argp_param_create(&pdr_file_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pdr_file_path_param, "f");
	doca_argp_param_set_long_name(pdr_file_path_param, "smf-config-file-path");
	doca_argp_param_set_description(pdr_file_path_param, "SMF JSON definitions file path");
	doca_argp_param_set_callback(pdr_file_path_param, smf_config_file_path_callback);
	doca_argp_param_set_type(pdr_file_path_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(pdr_file_path_param);
	result = doca_argp_register_param(pdr_file_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register UPF Acceleration JSON VXLAN definitions file path */
	result = doca_argp_param_create(&vxlan_file_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(vxlan_file_path_param, "x");
	doca_argp_param_set_long_name(vxlan_file_path_param, "vxlan-config-file-path");
	doca_argp_param_set_description(vxlan_file_path_param, "VXLAN JSON definitions file path");
	doca_argp_param_set_callback(vxlan_file_path_param, vxlan_config_file_path_callback);
	doca_argp_param_set_type(vxlan_file_path_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(vxlan_file_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register UPF Acceleration Aging time in seconds */
	result = doca_argp_param_create(&aging_time_sec_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(aging_time_sec_param, "t");
	doca_argp_param_set_long_name(aging_time_sec_param, "aging-time-sec");
	doca_argp_param_set_description(aging_time_sec_param, "Aging period in seconds");
	doca_argp_param_set_callback(aging_time_sec_param, aging_time_sec_callback);
	doca_argp_param_set_type(aging_time_sec_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(aging_time_sec_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register UPF Acceleration Number of packets before accelerating */
	result = doca_argp_param_create(&pkts_before_accel_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pkts_before_accel_param, "o");
	doca_argp_param_set_long_name(pkts_before_accel_param, "dpi-packet-threshold");
	doca_argp_param_set_description(
		pkts_before_accel_param,
		"Number of packets processed in software before accelerating to hardware, enabling DPI on initial packets");
	doca_argp_param_set_callback(pkts_before_accel_param, pkts_before_accel_callback);
	doca_argp_param_set_type(pkts_before_accel_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(pkts_before_accel_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register UPF Acceleration Number of packets before accelerating */
	result = doca_argp_param_create(&fixed_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(fixed_port_param, "p");
	doca_argp_param_set_long_name(fixed_port_param, "fixed-port");
	doca_argp_param_set_description(fixed_port_param, "UL port number in fixed-port mode");
	doca_argp_param_set_callback(fixed_port_param, fixed_port_callback);
	doca_argp_param_set_type(fixed_port_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(fixed_port_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t upf_accel_dpdk_config_num_ports(struct application_dpdk_config *dpdk_config)
{
	uint16_t nb_ports = rte_eth_dev_count_avail();

	if (nb_ports > UPF_ACCEL_PORTS_MAX) {
		DOCA_LOG_ERR("Invalid number (%u) of ports, max %u", nb_ports, UPF_ACCEL_PORTS_MAX);
		return DOCA_ERROR_INVALID_VALUE;
	}
	dpdk_config->port_config.nb_ports = nb_ports;

	return DOCA_SUCCESS;
}

/*
 * UPF Acceleration main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct upf_accel_config upf_accel_cfg = {
		.hw_aging_time_sec = UPF_ACCEL_HW_AGING_TIME_DEFAULT_SEC,
		.sw_aging_time_sec = UPF_ACCEL_SW_AGING_TIME_DEFAULT_SEC,
		.dpi_threshold = UPF_ACCEL_DEFAULT_DPI_THRESHOLD,
		.fixed_port = UPF_ACCEL_FIXED_PORT_NONE,
	};
	struct application_dpdk_config dpdk_config = {
		.port_config.enable_mbuf_metadata = 1,
	};
	struct upf_accel_fp_data *fp_data_arr = NULL;
	struct upf_accel_ctx upf_accel_ctx = {0};
	enum upf_accel_port port_id;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto upf_accel_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto upf_accel_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto upf_accel_exit;

	DOCA_LOG_INFO("Starting UPF Acceleration app pid %d", getpid());

	result = doca_argp_init(NULL, &upf_accel_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto upf_accel_exit;
	}
	doca_argp_set_dpdk_program(flow_init_dpdk);

	result = upf_accel_register_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register UPF Acceleration app parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse app input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&upf_accel_cfg.flow_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		goto dpdk_cleanup;
	}

	result = upf_accel_dpdk_config_num_ports(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to configure num ports");
		goto dpdk_cleanup;
	}

	/* Update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues");
		goto dpdk_cleanup;
	}

	if (!rte_flow_dynf_metadata_avail()) {
		DOCA_LOG_ERR("Dynamic flow metadata is not available");
		goto dpdk_ports_queues_cleanup;
	}

	result = upf_accel_smf_parse(&upf_accel_cfg);
	if (result != DOCA_SUCCESS)
		goto dpdk_ports_queues_cleanup;

	if (upf_accel_cfg.vxlan_config_file_path) {
		result = upf_accel_vxlan_parse(&upf_accel_cfg);
		if (result != DOCA_SUCCESS)
			goto dpdk_smf_cleanup;
	}

	upf_accel_ctx.num_ports = dpdk_config.port_config.nb_ports;
	upf_accel_ctx.num_queues = dpdk_config.port_config.nb_queues;
	upf_accel_ctx.resource.nr_counters = UPF_ACCEL_MAX_NUM_CONNECTIONS;
	upf_accel_ctx.num_shared_resources[DOCA_FLOW_SHARED_RESOURCE_METER] =
		upf_accel_calc_num_shared_meters(upf_accel_ctx.num_ports);
	upf_accel_ctx.num_shared_resources[DOCA_FLOW_SHARED_RESOURCE_COUNTER] =
		upf_accel_calc_num_shared_counters(upf_accel_ctx.num_ports);
	upf_accel_ctx.get_fwd_port = (upf_accel_ctx.num_ports == 1) ? upf_accel_single_port_get_fwd_port :
								      upf_accel_get_opposite_port;
	upf_accel_ctx.upf_accel_cfg = &upf_accel_cfg;
	for (port_id = 0; port_id < upf_accel_ctx.num_ports; port_id++) {
		upf_accel_ctx.static_entry_ctx[port_id].type = UPF_ACCEL_RULE_STATIC;
	}

	result = init_upf_accel(&upf_accel_ctx, &fp_data_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("init_upf_accel() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_vxlan_cleanup;
	}

	result = run_upf_accel(&upf_accel_ctx, fp_data_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("run_upf_accel() encountered an error: %s", doca_error_get_descr(result));
		goto upf_accel_deinit;
	}

	exit_status = EXIT_SUCCESS;
upf_accel_deinit:
	result = deinit_upf_accel(&upf_accel_ctx, fp_data_arr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("deinit_upf_accel() encountered an error: %s", doca_error_get_descr(result));
	}
dpdk_vxlan_cleanup:
	if (upf_accel_cfg.vxlan_config_file_path)
		upf_accel_vxlan_cleanup(&upf_accel_cfg);
dpdk_smf_cleanup:
	upf_accel_smf_cleanup(&upf_accel_cfg);
dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini_with_devs(dpdk_config.port_config.nb_ports);
argp_cleanup:
	doca_argp_destroy();
upf_accel_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("UPF Acceleration app finished successfully");
	else
		DOCA_LOG_INFO("UPF Acceleration app finished with errors");
	return exit_status;
}
