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
#include <doca_error.h>
#include <doca_log.h>

#include "upf_accel.h"
#include "upf_accel_smf_default_init.h"

DOCA_LOG_REGISTER(UPF_ACCEL::SMF_DEFAULT_INIT);

static struct upf_accel_pdr pdr1 = {
	.id = 1,
	.farid = 1,
	.urrids_num = 1,
	.urrids[0] = 1,
	.qerids_num = 1,
	.qerids[0] = 1,
	.pdi_si = UPF_ACCEL_PDR_PDI_SI_UL,
	.pdi_local_teid_start = 1000,
	.pdi_local_teid_end = 2000,
	.pdi_local_teid_ip.addr.v4 = 0xDEADBEEF,
	.pdi_local_teid_ip.mask.v4 = 0xFFFFFFFF,
	.pdi_local_teid_ip.ip_version = DOCA_FLOW_L3_TYPE_IP4,
	.pdi_ueip.addr.v4 = 0xBEEF0000,
	.pdi_ueip.mask.v4 = 0xFFFF0000,
	.pdi_ueip.ip_version = DOCA_FLOW_L3_TYPE_IP4,
	.pdi_qfi = 1,
	.pdi_sdf_from_port_range.from = 0,
	.pdi_sdf_from_port_range.to = 0xBEEF,
	.pdi_sdf_to_port_range.from = 0,
	.pdi_sdf_to_port_range.to = 0xBEEF,
};

static struct upf_accel_pdr pdr2 = {
	.id = 2,
	.farid = 2,
	.urrids_num = 1,
	.urrids[0] = 2,
	.qerids_num = 1,
	.qerids[0] = 2,
	.pdi_si = UPF_ACCEL_PDR_PDI_SI_DL,
	.pdi_ueip.addr.v6[0] = 0xDE,
	.pdi_ueip.addr.v6[1] = 0xAD,
	.pdi_ueip.addr.v6[2] = 0xBE,
	.pdi_ueip.addr.v6[3] = 0xEF,
	.pdi_ueip.mask.v6[0] = 0xFF,
	.pdi_ueip.mask.v6[1] = 0xFF,
	.pdi_ueip.mask.v6[2] = 0xFF,
	.pdi_ueip.mask.v6[3] = 0xFF,
	.pdi_ueip.ip_version = DOCA_FLOW_L3_TYPE_IP6,
	.pdi_sdf_from_port_range.from = 0,
	.pdi_sdf_from_port_range.to = 0xBEEF,
	.pdi_sdf_to_port_range.from = 0,
	.pdi_sdf_to_port_range.to = 0xBEEF,
};

static struct upf_accel_far far1 = {
	.id = 1,
};

static struct upf_accel_far far2 = {
	.id = 2,
	.fp_oh_teid = 1,
	.fp_oh_ip.addr.v4 = 0xDEADBEEF,
	.fp_oh_ip.mask.v4 = 0xFFFFFFFF,
	.fp_oh_ip.ip_version = DOCA_FLOW_L3_TYPE_IP4,
};

static struct upf_accel_urr urr1 = {
	.id = 1,
	.volume_quota_total_volume = 1000000000ULL, /* 1GB quota */
};

static struct upf_accel_urr urr2 = {
	.id = 2,
	.volume_quota_total_volume = 10000ULL, /* 10KB quota */
};

static struct upf_accel_qer qer1 = {
	.id = 1,
	.qfi = 0x14,
	.mbr_dl_mbr = 1000000000ULL, /* 1GB quota */
	.mbr_ul_mbr = 1000000000ULL, /* 1GB quota */
};

static struct upf_accel_qer qer2 = {
	.id = 2,
	.qfi = 0x14,
	.mbr_dl_mbr = 1000000000ULL, /* 1GB quota */
	.mbr_ul_mbr = 1000000000ULL, /* 1GB quota */
};

/*
 * Fill the PDRs with the default values
 *
 * @cfg [out]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_smf_dry_run_fill_pdrs(struct upf_accel_config *cfg)
{
	const size_t num_pdrs = 2;
	struct upf_accel_pdrs *pdrs =
		rte_zmalloc("UPF PDRs", sizeof(*pdrs) + sizeof(pdrs->arr_pdrs[0]) * num_pdrs, RTE_CACHE_LINE_SIZE);
	if (!pdrs) {
		DOCA_LOG_ERR("Failed to allocate PDR memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	pdrs->num_pdrs = num_pdrs;
	pdrs->arr_pdrs[0] = pdr1;
	pdrs->arr_pdrs[1] = pdr2;
	cfg->pdrs = pdrs;
	return DOCA_SUCCESS;
}

/*
 * Fill the FARs with the default values
 *
 * @cfg [out]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_smf_dry_run_fill_fars(struct upf_accel_config *cfg)
{
	const size_t num_fars = 2;
	struct upf_accel_fars *fars =
		rte_zmalloc("UPF FARs", sizeof(*fars) + sizeof(fars->arr_fars[0]) * num_fars, RTE_CACHE_LINE_SIZE);
	if (!fars) {
		DOCA_LOG_ERR("Failed to allocate FAR memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	fars->num_fars = num_fars;
	fars->arr_fars[0] = far1;
	fars->arr_fars[1] = far2;
	cfg->fars = fars;
	return DOCA_SUCCESS;
}

/*
 * Fill the URRs with the default values
 *
 * @cfg [out]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_smf_dry_run_fill_urrs(struct upf_accel_config *cfg)
{
	const size_t num_urrs = 2;
	struct upf_accel_urrs *urrs =
		rte_zmalloc("UPF URRs", sizeof(*urrs) + sizeof(urrs->arr_urrs[0]) * num_urrs, RTE_CACHE_LINE_SIZE);
	if (!urrs) {
		DOCA_LOG_ERR("Failed to allocate URR memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	urrs->num_urrs = num_urrs;
	urrs->arr_urrs[0] = urr1;
	urrs->arr_urrs[1] = urr2;
	cfg->urrs = urrs;
	return DOCA_SUCCESS;
}

/*
 * Fill the QERs with the default values
 *
 * @cfg [out]: UPF Acceleration configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t upf_accel_smf_dry_run_fill_qers(struct upf_accel_config *cfg)
{
	const size_t num_qers = 2;
	struct upf_accel_qers *qers =
		rte_zmalloc("UPF QERs", sizeof(*qers) + sizeof(qers->arr_qers[0]) * num_qers, RTE_CACHE_LINE_SIZE);
	if (!qers) {
		DOCA_LOG_ERR("Failed to allocate QER memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	qers->num_qers = num_qers;
	qers->arr_qers[0] = qer1;
	qers->arr_qers[1] = qer2;
	cfg->qers = qers;
	return DOCA_SUCCESS;
}

doca_error_t upf_accel_smf_dry_run_get(struct upf_accel_config *cfg)
{
	doca_error_t err;

	err = upf_accel_smf_dry_run_fill_pdrs(cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill PDRs");
		return err;
	}

	err = upf_accel_smf_dry_run_fill_fars(cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill FARs");
		goto err_pdr;
	}

	err = upf_accel_smf_dry_run_fill_urrs(cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill URRs");
		goto err_far;
	}

	err = upf_accel_smf_dry_run_fill_qers(cfg);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill QERs");
		goto err_urr;
	}

	return DOCA_SUCCESS;

err_urr:
	upf_accel_urr_cleanup(cfg);
err_far:
	upf_accel_far_cleanup(cfg);
err_pdr:
	upf_accel_pdr_cleanup(cfg);
	return err;
}
