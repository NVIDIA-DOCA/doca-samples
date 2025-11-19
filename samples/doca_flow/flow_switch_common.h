/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef FLOW_SWITCH_COMMON_H_
#define FLOW_SWITCH_COMMON_H_

#include <doca_flow.h>
#include <doca_dev.h>

#include <common.h>

#include "flow_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLOW_SWITCH_DEV_ARGS "dv_flow_en=2,fdb_def_rule_en=0,vport_match=1,repr_matching_en=0,dv_xmeta_en=4"

/* doca flow switch context */
struct flow_switch_ctx {
	struct flow_dev_ctx devs_ctx; /* Context for all device/port related data */
	bool is_expert;		      /* switch expert mode */
	void *usr_ctx;		      /* user context */
};

/*
 * Register DOCA Flow switch parameters
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t register_doca_flow_switch_params(void);

/*
 * Get number of DPDK ports created during EAL init according to user arguments.
 *
 * @return: number of created DPDK ports.
 */
uint8_t get_dpdk_nb_ports(void);

/*
 * Initialize DOCA Flow ports
 *
 * @devs_manager [in]: array of device bundles (doca device and representor devices)
 * @nb_managers [in]: number of managers to create ports for
 * @ports [in]: array of ports to create
 * @nb_ports [in]: number of ports to create
 * @actions_mem_size[in]: actions memory size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t init_doca_flow_switch_ports(struct flow_devs_manager devs_manager[],
					 int nb_managers,
					 struct doca_flow_port *ports[],
					 int nb_ports,
					 uint32_t actions_mem_size[],
					 struct flow_resources *resources);

#ifdef __cplusplus
}
#endif
#endif /* FLOW_SWITCH_COMMON_H_ */
