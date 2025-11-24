/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <time.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_dpdk.h>
#include <doca_ctx.h>
#include <doca_pe.h>

#include <samples/common.h>

#include "ipsec_ctx.h"
#include "flow_common.h"

DOCA_LOG_REGISTER(IPSEC_SECURITY_GW::ipsec_ctx);

#define SLEEP_IN_NANOS (10 * 1000) /* Sample the task every 10 microseconds  */

doca_error_t find_port_action_type_switch(int port_id, int *idx)
{
	struct rte_eth_dev_info dev_info;
	int ret;

	ret = rte_eth_dev_info_get(port_id, &dev_info);
	if (ret < 0) {
		DOCA_LOG_ERR("Failed getting DPDK port information: %s", strerror(-ret));
		return DOCA_ERROR_DRIVER;
	}

	assert(dev_info.switch_info.domain_id != RTE_ETH_DEV_SWITCH_DOMAIN_ID_INVALID);

	if (*dev_info.dev_flags & RTE_ETH_DEV_REPRESENTOR)
		*idx = UNSECURED_IDX;
	else
		*idx = SECURED_IDX;

	return DOCA_SUCCESS;
}

doca_error_t find_port_action_type_vnf(const struct ipsec_security_gw_config *app_cfg,
				       int port_id,
				       struct doca_dev **connected_dev,
				       int *idx)
{
	doca_error_t result;

	result = doca_dpdk_port_as_dev(port_id, connected_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to find DOCA device associated with port ID %d: %s",
			     port_id,
			     doca_error_get_descr(result));
		return result;
	}

	if (*connected_dev == app_cfg->objects.secured_dev.doca_dev) {
		*idx = SECURED_IDX;
		return DOCA_SUCCESS;
	} else if (*connected_dev == app_cfg->objects.unsecured_dev.doca_dev) {
		*idx = UNSECURED_IDX;
		return DOCA_SUCCESS;
	}

	return DOCA_ERROR_INVALID_VALUE;
}

doca_error_t ipsec_security_gw_close_devices(const struct ipsec_security_gw_config *app_cfg)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp_result;

	tmp_result = doca_dev_close(app_cfg->objects.secured_dev.doca_dev);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy secured DOCA dev: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	if (app_cfg->objects.unsecured_dev.is_representor) {
		tmp_result = doca_dev_rep_close(app_cfg->objects.unsecured_dev.doca_dev_rep);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy unsecured DOCA dev rep: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	} else {
		tmp_result = doca_dev_close(app_cfg->objects.unsecured_dev.doca_dev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy unsecured DOCA dev: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	return result;
}

doca_error_t ipsec_security_gw_init_devices(struct ipsec_security_gw_config *app_cfg)
{
	doca_error_t result;

	if (app_cfg->flow_mode == IPSEC_SECURITY_GW_VNF) {
		/* probe the opened doca devices with 'dv_flow_en=2' for HWS mode */
		result = doca_dpdk_port_probe(app_cfg->objects.secured_dev.doca_dev, "dv_flow_en=2,dv_xmeta_en=4");
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to probe dpdk port for secured port: %s", doca_error_get_descr(result));
			return result;
		}

		result = doca_dpdk_port_probe(app_cfg->objects.unsecured_dev.doca_dev, "dv_flow_en=2,dv_xmeta_en=4");
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to probe dpdk port for unsecured port: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		result = doca_dpdk_port_probe_with_representors(
			app_cfg->objects.secured_dev.doca_dev,
			"dv_flow_en=2,dv_xmeta_en=4,fdb_def_rule_en=0,vport_match=1,repr_matching_en=0",
			&app_cfg->objects.unsecured_dev.doca_dev_rep,
			1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to probe dpdk port for secured port and unsecured port representor: %s",
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}
