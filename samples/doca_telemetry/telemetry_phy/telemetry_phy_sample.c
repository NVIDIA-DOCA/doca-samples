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
#include <doca_dev.h>
#include <doca_telemetry_phy.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "telemetry_phy_sample.h"

DOCA_LOG_REGISTER(TELEMETRY_PHY::SAMPLE);

struct telemetry_phy_sample_objects {
	struct doca_telemetry_phy *telemetry_phy_obj;			      /* doca telemetry phy perf object*/
	struct doca_dev *dev;						      /* Doca device*/
	struct doca_telemetry_phy_pddr_operation_info *operation_info_struct; /* Structure that represent
											   the operation info */
	struct doca_telemetry_phy_pddr_module_info *module_info_struct;	      /* Structure that represent the module
										      info */
};

/*
 * Print operation info
 *
 * Print the contents of the extracted operation info.
 *
 * @operation_info_struct [in]: Extracted operation_info_struct to print
 *
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
doca_error_t telemetry_phy_print_operation_info(struct doca_telemetry_phy_pddr_operation_info *operation_info_struct)
{
	printf("\nOperational info\n");
	printf("----------------\n");

	/* Print proto active */
	printf("proto_active (%u):", operation_info_struct->proto_active);
	switch (operation_info_struct->proto_active) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROTO_ACTIVE_INFINIBAND:
		printf("\tINFINIBAND\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROTO_ACTIVE_ETHERNET:
		printf("\tETHERNET\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROTO_ACTIVE_NVLINK:
		printf("\tNVLINK\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print Operation info: proto_active=%u is invalid",
			     operation_info_struct->proto_active);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("neg_mode_active (%u):", operation_info_struct->neg_mode_active);
	switch (operation_info_struct->neg_mode_active) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_NEG_MODE_ACTIVE_PROTOCOL_WAS_NOT_NEGOTIATED:
		printf("\tPROTOCOL_WAS_NOT_NEGOTIATED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_NEG_MODE_ACTIVE_MLPN_REV0_NEGOTIATED:
		printf("\tMLPN_REV0_NEGOTIATED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_NEG_MODE_ACTIVE_CL73_ETHERNET_NEGOTIATED:
		printf("\tCL73_ETHERNET_NEGOTIATED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_NEG_MODE_ACTIVE_PROTOCOL_ACCORDING_TO_PARALLEL_DETECT:
		printf("\tPROTOCOL_ACCORDING_TO_PARALLEL_DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_NEG_MODE_ACTIVE_STANDARD_IB_NEGOTIATED:
		printf("\tSTANDARD_IB_NEGOTIATED\n");
		break;
	default:
		printf("\n");
		DOCA_LOG_WARN("Failed to print Operation info: neg_mode_active=%u is invalid",
			      operation_info_struct->neg_mode_active);
	}

	printf("pd_fsm_state (%u):", operation_info_struct->phy_mngr_fsm_state);
	switch (operation_info_struct->phy_mngr_fsm_state) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_DISABLED:
		printf("\tDISABLED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_OPEN_PORT:
		printf("\tOPEN_PORT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_POLLING:
		printf("\tPOLLING\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_ACTIVE:
		printf("\tACTIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_CLOSE_PORT:
		printf("\tCLOSE_PORT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_PHY_UP:
		printf("\tPHY_UP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_SLEEP:
		printf("\tSLEEP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_SIGNAL_DETECT:
		printf("\tSIGNAL_DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_RECEIVER_DETECT:
		printf("\tRECEIVER_DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_SYNC_PEER:
		printf("\tSYNC_PEER\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_NEGOTIATION:
		printf("\tNEGOTIATION\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_TRAINING:
		printf("\tTRAINING\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_SUBFSM_ACTIVE:
		printf("\tSUBFSM_ACTIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_PROTOCOL_DETECT:
		printf("\tPROTOCOL_DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PHY_MNGR_FSM_STATE_UNKOWN:
		printf("\tUNKOWN\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print operation info: phy_mngr_fsm_state =%u is invalid",
			     operation_info_struct->phy_mngr_fsm_state);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("eth_an_fsm_state (%u):", operation_info_struct->eth_an_fsm_state);
	switch (operation_info_struct->eth_an_fsm_state) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_ENABLE:
		printf("\tENABLE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_XMIT_DISABLE:
		printf("\tXMIT_DISABLE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_ABILITY_DETECT:
		printf("\tABILITY_DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_ACK_DETECT:
		printf("\tACK_DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_COMPLETE_ACK:
		printf("\tCOMPLETE_ACK\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_AN_GOOD_CHECK:
		printf("\tAN_GOOD_CHECK\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_AN_GOOD:
		printf("\tAN_GOOD\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_AN_FSM_NEXT_PAGE_WAIT:
		printf("\tNEXT_PAGE_WAIT\n");
		break;
	default:
		DOCA_LOG_WARN("Failed to print Operation info: eth_an_fsm_state=%u is invalid",
			      operation_info_struct->eth_an_fsm_state);
	}

	printf("ib_phy_fsm_state (%u):", operation_info_struct->ib_phy_fsm_state);
	switch (operation_info_struct->ib_phy_fsm_state) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_DISABLED:
		printf("\tIB_AN_FSM_DISABLED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_INITIALY:
		printf("\tIB_AN_FSM_INITIALY\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_RCVR_CFG:
		printf("\tIB_AN_FSM_RCVR_CFG\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_CFG_TEST:
		printf("\tIB_AN_FSM_CFG_TEST\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_WAIT_RMT_TEST:
		printf("\tIB_AN_FSM_WAIT_RMT_TEST\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_WAIT_CFG_ENHANCED:
		printf("\tIB_AN_FSM_WAIT_CFG_ENHANCED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_CFG_IDLE:
		printf("\tIB_AN_FSM_CFG_IDLE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_LINK_UP:
		printf("\tIB_AN_FSM_LINK_UP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_IB_PHY_FSM_STATE_IB_AN_FSM_POLLING:
		printf("\tIB_AN_FSM_POLLING\n");
		break;
	default:
		DOCA_LOG_WARN("Failed to print Operation info: ib_phy_fsm_state=%u is invalid",
			      operation_info_struct->ib_phy_fsm_state);
	}

	printf("loopback_mode(%u): ", operation_info_struct->loopback_mode);
	switch (operation_info_struct->loopback_mode) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_LOOPBACK_MODE_NO_LOOPBACK_ACTIVE:
		printf("\tNO_LOOPBACK_ACTIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_LOOPBACK_MODE_PHY_REMOTE_LOOPBACK:
		printf("\tPHY_REMOTE_LOOPBACK\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_LOOPBACK_MODE_PHY_LOCAL_LOOPBACK:
		printf("\tPHY_LOCAL_LOOPBACK\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_LOOPBACK_MODE_EXTERNAL_LOCAL_LOOPBACK:
		printf("\tEXTERNAL_LOCAL_LOOPBACK\n");
		break;
	default:
		DOCA_LOG_WARN("Failed to print Operation info: loopback_mode=%u is invalid",
			      operation_info_struct->loopback_mode);
	}

	printf("fec_mode_active (%u):", operation_info_struct->fec_mode_active);
	switch (operation_info_struct->fec_mode_active) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_NO_FEC:
		printf("\tNO_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_FIRECODE_FEC:
		printf("\tFIRECODE_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_STANDARD_RS_FEC:
		printf("\tSTANDARD_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_STANDARD_LL_RS_FEC:
		printf("\tSTANDARD_LL_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_INTERLEAVED_QUAD_RS_FEC:
		printf("\tINTERLEAVED_QUAD_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_INTERLEAVED_STANDARD_RS:
		printf("\tINTERLEAVED_STANDARD_RS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_STANDARD_RS:
		printf("\tSTANDARD_RS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_ETHERNET_CONSORTIUM_LL_50G_RS_FEC:
		printf("\tETHERNET_CONSORTIUM_LL_50G_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_INTERLEAVED_ETHERNET_CONSORTIUM_LL_50G_RS_FEC:
		printf("\tINTERLEAVED_ETHERNET_CONSORTIUM_LL_50G_RS_FEC\n");
		break;
	default:
		DOCA_LOG_WARN("Failed to print Operation info: fec_mode_active=%u is invalid",
			      operation_info_struct->fec_mode_active);
	}

	printf("fec_mode_request (%u):", operation_info_struct->fec_mode_request);
	switch (operation_info_struct->fec_mode_request) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_NO_FEC:
		printf("\tNO_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_FIRECODE_FEC:
		printf("\tFIRECODE_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_STANDARD_RS_FEC:
		printf("\tSTANDARD_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_STANDARD_LL_RS_FEC:
		printf("\tSTANDARD_LL_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_INTERLEAVED_QUAD_RS_FEC:
		printf("\tINTERLEAVED_QUAD_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_INTERLEAVED_STANDARD_RS:
		printf("\tINTERLEAVED_STANDARD_RS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_STANDARD_RS:
		printf("\tSTANDARD_RS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_ETHERNET_CONSORTIUM_LL_50G_RS_FEC:
		printf("\tETHERNET_CONSORTIUM_LL_50G_RS_FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_FEC_MODE_ACTIVE_INTERLEAVED_ETHERNET_CONSORTIUM_LL_50G_RS_FEC:
		printf("\tINTERLEAVED_ETHERNET_CONSORTIUM_LL_50G_RS_FEC\n");
		break;
	default:
		DOCA_LOG_WARN("Failed to print Operation info: fec_mode_request=%u is invalid",
			      operation_info_struct->fec_mode_request);
	}

	printf("profile_fec_in_use (%u): ", operation_info_struct->profile_fec_in_use);
	if (operation_info_struct->profile_fec_in_use == 0) {
		printf("N/A\n");
	} else {
		printf("\n");
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_IB_SPEC_LEGACY) {
			printf("\t\tIB_SPEC_LEGACY\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_INTERNAL_PORTS) {
			printf("\t\tINTERNAL_PORTS\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_PASSIVE_COPPER_SHORT) {
			printf("\t\tPASSIVE_COPPER_SHORT\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_PASSIVE_COPPER_MEDIUM) {
			printf("\t\tPASSIVE_COPPER_MEDIUM\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_PASSIVE_COPPER_LONG) {
			printf("\t\tPASSIVE_COPPER_LONG\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_ACTIVE_OPTIC_COPPER_SHORT) {
			printf("\t\tACTIVE_OPTIC_COPPER_SHORT\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_OPTIC_LONG) {
			printf("\t\tOPTIC_LONG\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_NO_FEC) {
			printf("\t\tNO_FEC\n");
		}
		if (operation_info_struct->profile_fec_in_use &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROFILE_FEC_IN_USE_FEC_ON) {
			printf("\t\tFEC_ON\n");
		}
	}

	printf("eth_25g_50g_fec_support (%u): ", operation_info_struct->eth_25g_50g_fec_support);
	if (operation_info_struct->eth_25g_50g_fec_support == 0) {
		printf("N/A\n");
	} else {
		printf("\n");
		if (operation_info_struct->eth_25g_50g_fec_support &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_25G_50G_FEC_NO_FEC) {
			printf("\t\tNO_FEC\n");
		}
		if (operation_info_struct->eth_25g_50g_fec_support &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_25G_50G_FEC_FIRECODE_FEC) {
			printf("\t\tFIRECODE_FEC\n");
		}
		if (operation_info_struct->eth_25g_50g_fec_support &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_25G_50G_FEC_STANDARD_FEC) {
			printf("\t\tSTANDARD_FEC\n");
		}
	}

	printf("eth_100g_fec_support (%u): ", operation_info_struct->eth_100g_fec_support);
	if (operation_info_struct->eth_100g_fec_support == 0) {
		printf("N/A\n");
	} else {
		printf("\n");
		if (operation_info_struct->eth_100g_fec_support &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_100G_FEC_NO_FEC) {
			printf("\t\tNO_FEC\n");
		}
		if (operation_info_struct->eth_100g_fec_support &
		    DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_ETH_100G_FEC_STANDARD_FEC) {
			printf("\t\tSTANDARD_FEC\n");
		}
	}

	printf("eth_an_link_enabled: %u\n", operation_info_struct->eth_an_link_enabled);

	/* Print proto active */
	switch (operation_info_struct->proto_active) {
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROTO_ACTIVE_ETHERNET:
		printf("phy_manager_link_enabled: %u\n",
		       operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_eth
			       .phy_manager_link_eth_enabled);
		printf("core_to_phy_link_enabled: %u\n",
		       operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_eth
			       .core_to_phy_link_eth_enabled);
		printf("cable_proto_cap: %u\n",
		       operation_info_struct->cable_proto_cap.pddr_cable_cap_eth.cable_ext_eth_proto_cap);
		printf("link_active: %u\n", operation_info_struct->link_active.pddr_link_active_eth.link_eth_active);
		printf("pd_link_enabled: %u\n",
		       operation_info_struct->pd_link_enabled.pd_link_eth_enabled.link_eth_active);
		printf("phy_hst_link_enabled: %u\n",
		       operation_info_struct->phy_hst_link_enabled.hst_link_eth_enabled.link_eth_active);
		break;
	case DOCA_TELEMETRY_PHY_PDDR_OPERATION_INFO_PROTO_ACTIVE_INFINIBAND:
		printf("phy_manager_link_width_enabled: %u\n",
		       operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
			       .phy_manager_link_width_enabled);
		printf("phy_manager_link_proto_enabled (%u): ",
		       operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
			       .phy_manager_link_proto_enabled);

		if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
			    .phy_manager_link_proto_enabled == 0) {
			printf("N/A\n");
		} else {
			printf("\n");
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_SDR) {
				printf("\t\tSDR\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_DDR) {
				printf("\t\tDDR\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_QDR) {
				printf("\t\tQDR\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_FDR10) {
				printf("\t\tFDR10\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_FDR) {
				printf("\t\tFDR\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_EDR) {
				printf("\t\tEDR\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_HDR) {
				printf("\t\tHDR\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_NDR) {
				printf("\t\tNDR\n");
			}
			if (operation_info_struct->phy_manager_link_enabled.pddr_phy_manager_link_enabled_ib
				    .phy_manager_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_MANAGER_LINK_PROTO_ENABLED_XDR) {
				printf("\t\tXDR\n");
			}
		}

		printf("core_to_phy_link_width_enabled: %u\n",
		       operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
			       .core_to_phy_link_width_enabled);
		printf("core_to_phy_link_proto_enabled (%u): ",
		       operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
			       .core_to_phy_link_proto_enabled);

		if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
			    .core_to_phy_link_proto_enabled == 0) {
			printf("N/A\n");
		} else {
			printf("\n");
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_SDR) {
				printf("\t\tSDR\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_DDR) {
				printf("\t\tDDR\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_QDR) {
				printf("\t\tQDR\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_FDR10) {
				printf("\t\tFDR10\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_FDR) {
				printf("\t\tFDR\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_EDR) {
				printf("\t\tEDR\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_HDR) {
				printf("\t\tHDR\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_NDR) {
				printf("\t\tNDR\n");
			}
			if (operation_info_struct->core_to_phy_link_enabled.pddr_c2p_link_enabled_ib
				    .core_to_phy_link_proto_enabled &
			    DOCA_TELEMETRY_PHY_PDDR_C2P_LINK_ENABLED_IB_CORE_TO_PHY_LINK_PROTO_ENABLED_XDR) {
				printf("\t\tXDR\n");
			}
		}

		printf("cable_link_width_cap: %u\n",
		       operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_width_cap);
		printf("cable_link_speed_cap (%u): ",
		       operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap);

		if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap == 0) {
			printf("N/A\n");
		} else {
			printf("\n");
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_SDR) {
				printf("\t\tSDR\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_DDR) {
				printf("\t\tDDR\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_QDR) {
				printf("\t\tQDR\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_FDR10) {
				printf("\t\tFDR10\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_FDR) {
				printf("\t\tFDR\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_EDR) {
				printf("\t\tEDR\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_HDR) {
				printf("\t\tHDR\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_NDR) {
				printf("\t\tNDR\n");
			}
			if (operation_info_struct->cable_proto_cap.pddr_cable_cap_ib.cable_link_speed_cap &
			    DOCA_TELEMETRY_PHY_PDDR_CABLE_CAP_IB_CABLE_LINK_SPEED_CAP_XDR) {
				printf("\t\tXDR\n");
			}
		}

		printf("pddr_link_width_active: %u\n",
		       operation_info_struct->link_active.pddr_link_active_ib.link_width_active);
		printf("pddr_link_speed_active (%u): ",
		       operation_info_struct->link_active.pddr_link_active_ib.link_speed_active);

		if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active == 0) {
			printf("N/A\n");
		} else {
			printf("\n");
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_SDR) {
				printf("\t\tSDR\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_DDR) {
				printf("\t\tDDR\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_QDR) {
				printf("\t\tQDR\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_FDR10) {
				printf("\t\tFDR10\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_FDR) {
				printf("\t\tFDR\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_EDR) {
				printf("\t\tEDR\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_HDR) {
				printf("\t\tHDR\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_NDR) {
				printf("\t\tNDR\n");
			}
			if (operation_info_struct->link_active.pddr_link_active_ib.link_speed_active &
			    DOCA_TELEMETRY_PHY_PDDR_LINK_ACTIVE_IB_LINK_SPEED_ACTIVE_XDR) {
				printf("\t\tXDR\n");
			}
		}

		printf("pd_link_width_active: %u\n",
		       operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_width_active);
		printf("pd_link_speed_active (%u): ",
		       operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active);

		if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active == 0) {
			printf("N/A\n");
		} else {
			printf("\n");
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_SDR) {
				printf("\t\tSDR\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_DDR) {
				printf("\t\tDDR\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_QDR) {
				printf("\t\tQDR\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_FDR10) {
				printf("\t\tFDR10\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_FDR) {
				printf("\t\tFDR\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_EDR) {
				printf("\t\tEDR\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_HDR) {
				printf("\t\tHDR\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_NDR) {
				printf("\t\tNDR\n");
			}
			if (operation_info_struct->pd_link_enabled.pd_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_PD_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_XDR) {
				printf("\t\tXDR\n");
			}
		}

		printf("phy_hst_link_width_active: %u\n",
		       operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_width_active);
		printf("phy_hst_link_speed_active (%u): ",
		       operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active);

		if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active == 0) {
			printf("N/A\n");
		} else {
			printf("\n");
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_SDR) {
				printf("\t\tSDR\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_DDR) {
				printf("\t\tDDR\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_QDR) {
				printf("\t\tQDR\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_FDR10) {
				printf("\t\tFDR10\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_FDR) {
				printf("\t\tFDR\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_EDR) {
				printf("\t\tEDR\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_HDR) {
				printf("\t\tHDR\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_NDR) {
				printf("\t\tNDR\n");
			}
			if (operation_info_struct->phy_hst_link_enabled.hst_link_ib_enabled.link_speed_active &
			    DOCA_TELEMETRY_PHY_HST_LINK_IB_ENABLED_LINK_SPEED_ACTIVE_XDR) {
				printf("\t\tXDR\n");
			}
		}
		break;
	default:
		DOCA_LOG_ERR("Failed to print Operation info: proto_active vendor=%u is invalid",
			     operation_info_struct->proto_active);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Print monitor info dp_st_lane
 *
 * Print the contents of the extracted Module info dp_st_lane.
 *
 * @dp_st_lane_idx [in]: Extracted module_info dp_st_lane index to print
 * @dp_st_lane [in]: Extracted module_info dp_st_lane to print
 *
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
void telemetry_phy_print_PDDR_module_info_dp_st_lane(uint8_t dp_st_lane_idx, uint8_t dp_st_lane)
{
	printf("dp_st_lane_%u (%u): ", dp_st_lane_idx, dp_st_lane);
	if (dp_st_lane == 0) {
		printf("N/A\n");
	} else {
		printf("\n");
		if (dp_st_lane & DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_DP_ST_LANE_DPDEACTIVATED) {
			printf("\t\tDP_ST_LANE_DPDEACTIVATED\n");
		}
		if (dp_st_lane & DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_DP_ST_LANE_DPINIT) {
			printf("\t\tDP_ST_LANE_DPINIT\n");
		}
		if (dp_st_lane & DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_DP_ST_LANE_DPDEINIT) {
			printf("\t\tDP_ST_LANE_DPDEINIT\n");
		}
		if (dp_st_lane & DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_DP_ST_LANE_DPACTIVATED) {
			printf("\t\tDP_ST_LANE_DPACTIVATED\n");
		}
		if (dp_st_lane & DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_DP_ST_LANE_DPTXTURNON) {
			printf("\t\tDP_ST_LANE_DPTXTURNON\n");
		}
		if (dp_st_lane & DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_DP_ST_LANE_DPTXTURNOFF) {
			printf("\t\tDP_ST_LANE_DPTXTURNOFF\n");
		}
		if (dp_st_lane & DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_DP_ST_LANE_DPINITIALIZED) {
			printf("\t\tDP_ST_LANE_DPINITIALIZED\n");
		}
	}
}

/*
 * Print monitor info
 *
 * Print the contents of the extracted Module info.
 *
 * @module_info_struct [in]: Extracted module info struct to print
 *
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
doca_error_t telemetry_phy_print_module_info(struct doca_telemetry_phy_pddr_module_info *module_info_struct)
{
	uint8_t is_QSFP = false;
	uint8_t is_CMIS = false;

	printf("\nModule info\n");
	printf("-----------\n");

	printf("module_info_struct->cable_identifier (%u):", module_info_struct->cable_identifier);
	switch (module_info_struct->cable_identifier) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP28:
		printf("\tQSFP28\n");
		is_QSFP = true;
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_PLUS:
		printf("\tQSFP_PLUS\n");
		is_QSFP = true;
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_SFP28_OR_SFP_PLUS:
		printf("\tSFP28_OR_SFP_PLUS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSA_QSFP_SFP:
		printf("\tQSA_QSFP_SFP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_BLACKPLANE:
		printf("\tBLACKPLANE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_SFP_DD:
		printf("\tSFP_DD\n");
		is_CMIS = true;
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_DD:
		printf("\tQSFP_DD\n");
		is_QSFP = true;
		is_CMIS = true;
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_CMIS:
		printf("\tQSFP_CMIS\n");
		is_CMIS = true;
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_OSFP:
		printf("\tOSFP\n");
		is_CMIS = true;
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_C2C:
		printf("\tC2C\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_DSFP:
		printf("\tDSFP\n");
		is_CMIS = true;
		is_QSFP = true;
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_SPLIT_CABLE:
		printf("\tQSFP_SPLIT_CABLE\n");
		break;
	default:
		printf("\n");
		DOCA_LOG_ERR("Failed to print module info: cable identifier=%u is invalid",
			     module_info_struct->cable_identifier);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* QSFP: Byte 147 per SFF-8636.
	 * SFP: SFP+ Cable Technology:
	 *      byte8 per SFF-8472:
	 *      Bit 3 - Active Cable
	 *      Bit 2 - Passive Cable
	 * CMIS based (QSFP-DD / OSFP/ SFP-DD/OE): Byte 212
	 * */
	uint8_t cable_technology_test;
	if (is_CMIS) {
		cable_technology_test = module_info_struct->cable_technology;
	} else if (is_QSFP) {
		cable_technology_test = (module_info_struct->cable_technology & 0xf0) >> 4;
	} else {
		cable_technology_test = module_info_struct->cable_technology & 0x0f;
	}
	printf("cable_technology (%u):", module_info_struct->cable_technology);
	switch (cable_technology_test) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_VCSEL_850NM:
		printf("\tVCSEL_850NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_VCSEL_1310NM:
		printf("\tVCSEL_1310NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_VCSEL_1550NM:
		printf("\tVCSEL_1550NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_FP_LASER_1310NM:
		printf("\tFP_LASER_1310NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_DFB_LASER_1310NM:
		printf("\tDFB_LASER_1310NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_DFB_LASER_1550NM:
		printf("\tDFB_LASER_1550NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_EML_1310NM:
		printf("\tEML_1310NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_EML_1550NM:
		printf("\tEML_1550NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_OTHERS:
		printf("\tOTHERS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_DFB_LASER_1490NM:
		printf("\tDFB_LASER_1490NM\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_PASSIVE_COPPER_CABLE_UNEQUALIZED:
		printf("\tPASSIVE_COPPER_CABLE_UNEQUALIZED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_PASSIVE_COPPER_CABLE_EQUALIZED:
		printf("\tPASSIVE_COPPER_CABLE_EQUALIZED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_COPPER_CABLE_NEAR_END_AND_FAR_END_LIMITING_ACTIVE_EQUAILIZER:
		printf("\tCOPPER_CABLE_NEAR_END_AND_FAR_END_LIMITING_ACTIVE_EQUAILIZER\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_COPPER_CABLE_FAR_END_LIMITING_ACTIVE_EQUAILIZER:
		printf("\tCOPPER_CABLE_FAR_END_LIMITING_ACTIVE_EQUAILIZER\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_COPPER_CABLE_NEAR_END_LIMITING_ACTIVE_EQUIALIZER:
		printf("\tCOPPER_CABLE_NEAR_END_LIMITING_ACTIVE_EQUIALIZER\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_COPPER_CABLE_LINEAR_ACTIVE_EQUALIZERS:
		printf("\tCOPPER_CABLE_LINEAR_ACTIVE_EQUALIZERS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_C_BAND_TUNABLE_LASER:
		printf("\tC_BAND_TUNABLE_LASER\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_L_BAND_TUNABLE_LASER:
		printf("\tL_BAND_TUNABLE_LASER\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_COPPER_CABLE_NEAR_END_AND_FAR_END_LINEAR_ACTIVE_EQUALIZERS:
		printf("\tCOPPER_CABLE_NEAR_END_AND_FAR_END_LINEAR_ACTIVE_EQUALIZERS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_COPPER_CABLE_FAR_END_LINEAR_ACTIVE_EQUALIZERS:
		printf("\tCOPPER_CABLE_FAR_END_LINEAR_ACTIVE_EQUALIZERS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TECHNOLOGY_COPPER_CABLE_NEAR_END_LINEAR_ACTIVE_EQUALIZERS:
		printf("\tCOPPER_CABLE_NEAR_END_LINEAR_ACTIVE_EQUALIZERS\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: cable_technology=%u is invalid",
			     module_info_struct->cable_technology);
		return DOCA_ERROR_INVALID_VALUE;
	}

	uint8_t cable_breakout_test;
	if (is_CMIS) {
		cable_breakout_test = module_info_struct->cable_breakout;
	} else if (is_QSFP) {
		cable_breakout_test = (module_info_struct->cable_breakout & 0xf0) >> 4;
	} else {
		cable_breakout_test = module_info_struct->cable_breakout & 0xf0;
	}
	printf("cable_breakout (%u):", module_info_struct->cable_breakout);
	switch (cable_breakout_test) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_UNSPECIFIED:
		printf("\tUNSPECIFIED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_XX:
		printf("\tXX_XX\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_2QSFP_OR_2XX_D4L:
		printf("\tXX_2QSFP_OR_2XX_D4L\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_4DSFP_OR_4QSFP_D2L:
		printf("\tXX_4DSFP_OR_4QSFP_D2L\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_8SFP:
		printf("\tXX_8SFP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_D4L_XX_D4L_OR_QSFP:
		printf("\tXX_D4L_XX_D4L_OR_QSFP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_D4L_2XX_D2L_OR_2SFP:
		printf("\tXX_D4L_2XX_D2L_OR_2SFP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_D4L_4SFP:
		printf("\tXX_D4L_4SFP\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_2L_XX:
		printf("\tXX_2L_XX\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_BREAKOUT_XX_2L_2SFP:
		printf("\tXX_2L_2SFP\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: cable_breakout=%u is invalid",
			     module_info_struct->cable_breakout);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("ext_ethernet_compliance_code: %u\n", module_info_struct->ext_ethernet_compliance_code);
	printf("ethernet_compliance_code: %u\n", module_info_struct->ethernet_compliance_code);
	printf("cable_type (%u):", module_info_struct->cable_type);
	switch (module_info_struct->cable_type) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TYPE_UNIDENTIFIED:
		printf("\tUNIDENTIFIED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TYPE_ACTIVE_CABLE:
		printf("\tACTIVE_CABLE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TYPE_OPTICAL_MODULE:
		printf("\tOPTICAL_MODULE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TYPE_PASSIVE_COPPER_CABLE:
		printf("\tPASSIVE_COPPER_CABLE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TYPE_CABLE_UNPLUGGED:
		printf("\tCABLE_UNPLUGGED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_TYPE_TWISTED_PAIR:
		printf("\tTWISTED_PAIR\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: cable_type=%u is invalid", module_info_struct->cable_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("cable_vendor (%u):", module_info_struct->cable_vendor);
	switch (module_info_struct->cable_vendor) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_VENDOR_OTHER:
		printf("\tOTHER\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_VENDOR_MELLANOX:
		printf("\tMELLANOX\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_VENDOR_KNOWN_OUI:
		printf("\tKNOWN_OUI\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_VENDOR_NVIDIA:
		printf("\tNVIDIA\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: cable_vendor=%u is invalid",
			     module_info_struct->cable_vendor);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("cable_length: %u\n", module_info_struct->cable_length);

	switch (module_info_struct->cable_identifier) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP28:
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_PLUS:
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_SPLIT_CABLE:
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_SFP28_OR_SFP_PLUS:
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSA_QSFP_SFP:
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_DSFP:
		printf("max_power_consumption_SFP_QSFP: %u\n",
		       module_info_struct->cable_power_class.max_power_consumption_SFP_QSFP);
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_SFP_DD:
		printf("max_power_consumption_SFP_DD: %u\n",
		       module_info_struct->cable_power_class.max_power_consumption_SFP_DD);
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_DD:
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_QSFP_CMIS:
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_CABLE_IDENTIFIER_OSFP:
		printf("max_power_consumption_QSFP_DD_OSFP: %u\n",
		       module_info_struct->cable_power_class.max_power_consumption_QSFP_DD_OSFP);
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: cable_identifier=%u is invalid",
			     module_info_struct->cable_identifier);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	printf("max_power: %u\n", module_info_struct->max_power);
	printf("cable_rx_amp: %u\n", module_info_struct->cable_rx_amp);
	printf("cable_rx_emphasis: %u\n", module_info_struct->cable_rx_emphasis);
	printf("cable_attenuation_25g: %u\n", module_info_struct->cable_attenuation_25g);
	printf("cable_attenuation_12g: %u\n", module_info_struct->cable_attenuation_12g);
	printf("cable_attenuation_7g: %u\n", module_info_struct->cable_attenuation_7g);
	printf("cable_attenuation_5g: %u\n", module_info_struct->cable_attenuation_5g);
	printf("cable_rx_post_emphasis: %u\n", module_info_struct->cable_rx_post_emphasis);
	printf("rx_cdr_cap: %u\n", module_info_struct->rx_cdr_cap);
	printf("tx_cdr_cap: %u\n", module_info_struct->tx_cdr_cap);
	printf("rx_cdr_state: %u\n", module_info_struct->rx_cdr_state);
	printf("tx_cdr_state: %u\n", module_info_struct->tx_cdr_state);
	printf("vendor_name: %s\n", module_info_struct->vendor_name);
	printf("vendor_pn: %s\n", module_info_struct->vendor_pn);
	printf("vendor_rev: %u\n", module_info_struct->vendor_rev);
	printf("fw_version: %u\n", module_info_struct->fw_version);
	printf("vendor_sn: %s\n", module_info_struct->vendor_sn);
	printf("temperature: %u\n", module_info_struct->temperature);
	printf("voltage: %u\n", module_info_struct->voltage);
	printf("rx_power_lane0: %u\n", module_info_struct->rx_power_lane0);
	printf("rx_power_lane1: %u\n", module_info_struct->rx_power_lane1);
	printf("rx_power_lane2: %u\n", module_info_struct->rx_power_lane2);
	printf("rx_power_lane3: %u\n", module_info_struct->rx_power_lane3);
	printf("rx_power_lane4: %u\n", module_info_struct->rx_power_lane4);
	printf("rx_power_lane5: %u\n", module_info_struct->rx_power_lane5);
	printf("rx_power_lane6: %u\n", module_info_struct->rx_power_lane6);
	printf("rx_power_lane7: %u\n", module_info_struct->rx_power_lane7);
	printf("tx_power_lane0: %u\n", module_info_struct->tx_power_lane0);
	printf("tx_power_lane1: %u\n", module_info_struct->tx_power_lane1);
	printf("tx_power_lane2: %u\n", module_info_struct->tx_power_lane2);
	printf("tx_power_lane3: %u\n", module_info_struct->tx_power_lane3);
	printf("tx_power_lane4: %u\n", module_info_struct->tx_power_lane4);
	printf("tx_power_lane5: %u\n", module_info_struct->tx_power_lane5);
	printf("tx_power_lane6: %u\n", module_info_struct->tx_power_lane6);
	printf("tx_power_lane7: %u\n", module_info_struct->tx_power_lane7);
	printf("tx_bias_lane0: %u\n", module_info_struct->tx_bias_lane0);
	printf("tx_bias_lane1: %u\n", module_info_struct->tx_bias_lane1);
	printf("tx_bias_lane2: %u\n", module_info_struct->tx_bias_lane2);
	printf("tx_bias_lane3: %u\n", module_info_struct->tx_bias_lane3);
	printf("tx_bias_lane4: %u\n", module_info_struct->tx_bias_lane4);
	printf("tx_bias_lane5: %u\n", module_info_struct->tx_bias_lane5);
	printf("tx_bias_lane6: %u\n", module_info_struct->tx_bias_lane6);
	printf("tx_bias_lane7: %u\n", module_info_struct->tx_bias_lane7);
	printf("temperature_high_th: %u\n", module_info_struct->temperature_high_th);
	printf("temperature_low_th: %u\n", module_info_struct->temperature_low_th);
	printf("voltage_high_th: %u\n", module_info_struct->voltage_high_th);
	printf("voltage_low_th: %u\n", module_info_struct->voltage_low_th);
	printf("rx_power_high_th: %u\n", module_info_struct->rx_power_high_th);
	printf("rx_power_low_th: %u\n", module_info_struct->rx_power_low_th);
	printf("tx_power_high_th: %u\n", module_info_struct->tx_power_high_th);
	printf("tx_power_low_th: %u\n", module_info_struct->tx_power_low_th);
	printf("tx_bias_high_th: %u\n", module_info_struct->tx_bias_high_th);
	printf("tx_bias_low_th: %u\n", module_info_struct->tx_bias_low_th);
	printf("module_st (%u):", module_info_struct->module_st);
	switch (module_info_struct->module_st) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_MODULE_ST_RESERVED:
		printf("\tRESERVED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_MODULE_ST_LOWPWR_STATE:
		printf("\tLOWPWR_STATE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_MODULE_ST_PWRUP_STATE:
		printf("\tPWRUP_STATE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_MODULE_ST_READY_STATE:
		printf("\tREADY_STATE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_MODULE_ST_PWRDN_STATE:
		printf("\tPWRDN_STATE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_MODULE_ST_FAULT_STATE:
		printf("\tFAULT_STATE\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: module_st=%u is invalid", module_info_struct->module_st);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("rx_power_type (%u):", module_info_struct->rx_power_type);
	switch (module_info_struct->rx_power_type) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_RX_POWER_TYPE_OMA:
		printf("\tOMA\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_RX_POWER_TYPE_AVERAGE_POWER:
		printf("\tAVERAGE_POWER\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: rx_power_type=%u is invalid",
			     module_info_struct->rx_power_type);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("did_cap: %u\n", module_info_struct->did_cap);
	printf("rx_output_valid_cap: %u\n", module_info_struct->rx_output_valid_cap);
	printf("smf_length: %u\n", module_info_struct->smf_length);
	printf("wavelength: %u\n", module_info_struct->wavelength);
	printf("active_set_host_compliance_code: %u\n", module_info_struct->active_set_host_compliance_code);
	printf("active_set_media_compliance_code: %u\n", module_info_struct->active_set_media_compliance_code);
	printf("tx_bias_scaling_factor (%u):", module_info_struct->tx_bias_scaling_factor);
	switch (module_info_struct->tx_bias_scaling_factor) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_TX_BIAS_SCALING_FACTOR_MULTIPLY_1X:
		printf("\tMULTIPLY_1X\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_TX_BIAS_SCALING_FACTOR_MULTIPLY_2X:
		printf("\tMULTIPLY_2X\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_TX_BIAS_SCALING_FACTOR_MULTIPLY_4X:
		printf("\tMULTIPLY_4X\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: tx_bias_scaling_factor=%u is invalid",
			     module_info_struct->tx_bias_scaling_factor);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("ib_compliance_code (%u): ", module_info_struct->ib_compliance_code);
	if (module_info_struct->ib_compliance_code == 0) {
		printf("N/A\n");
	} else {
		printf("\n");
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_SDR) {
			printf("\t\tSDR\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_DDR) {
			printf("\t\tDDR\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_QDR) {
			printf("\t\tQDR\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_FDR10) {
			printf("\t\tFDR10\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_FDR) {
			printf("\t\tFDR\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_EDR) {
			printf("\t\tEDR\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_HDR) {
			printf("\t\tHDR\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_NDR) {
			printf("\t\tNDR\n");
		}
		if (module_info_struct->ib_compliance_code &
		    DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_IB_COMPLIANCE_CODE_XDR) {
			printf("\t\tXDR\n");
		}
	}

	printf("nbr250: %u\n", module_info_struct->nbr250);
	printf("nbr100: %u\n", module_info_struct->nbr100);
	printf("monitor_cap_mask: %u\n", module_info_struct->monitor_cap_mask);
	printf("ib_width: %u\n", module_info_struct->ib_width);

	telemetry_phy_print_PDDR_module_info_dp_st_lane(0, module_info_struct->dp_st_lane_0);
	telemetry_phy_print_PDDR_module_info_dp_st_lane(1, module_info_struct->dp_st_lane_1);
	telemetry_phy_print_PDDR_module_info_dp_st_lane(2, module_info_struct->dp_st_lane_2);
	telemetry_phy_print_PDDR_module_info_dp_st_lane(3, module_info_struct->dp_st_lane_3);
	telemetry_phy_print_PDDR_module_info_dp_st_lane(4, module_info_struct->dp_st_lane_4);
	telemetry_phy_print_PDDR_module_info_dp_st_lane(5, module_info_struct->dp_st_lane_5);
	telemetry_phy_print_PDDR_module_info_dp_st_lane(6, module_info_struct->dp_st_lane_6);
	telemetry_phy_print_PDDR_module_info_dp_st_lane(7, module_info_struct->dp_st_lane_7);

	printf("length_om2: %u\n", module_info_struct->length_om2);
	printf("length_om3: %u\n", module_info_struct->length_om3);
	printf("length_om4: %u\n", module_info_struct->length_om4);
	printf("length_om5: %u\n", module_info_struct->length_om5);
	printf("length_om1: %u\n", module_info_struct->length_om1);
	printf("wavelength_tolerance: %u\n", module_info_struct->wavelength_tolerance);
	printf("memory_map_rev: %u\n", module_info_struct->memory_map_rev);
	printf("memory_map_compliance: %u\n", module_info_struct->memory_map_compliance);
	printf("date_code.hi: %u\n", module_info_struct->date_code.hi);
	printf("date_code.lo: %u\n", module_info_struct->date_code.lo);
	printf("connector_type: %u\n", module_info_struct->connector_type);
	printf("vendor_oui: %u\n", module_info_struct->vendor_oui);
	printf("tx_input_freq_sync (%u):", module_info_struct->tx_input_freq_sync);
	switch (module_info_struct->tx_input_freq_sync) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_TX_INPUT_FREQ_SYNC_TX_INPUT_LANES_1_8:
		printf("\tLANES_1_8\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_TX_INPUT_FREQ_SYNC_TX_INPUT_LANES_1_4_AND_5:
		printf("\tLANES_1_4_AND_5\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_TX_INPUT_FREQ_SYNC_TX_INPUT_LANES_1_2_AND_3_4_AND_5_6_AND_7_8:
		printf("\tLANES_1_2_AND_3_4_AND_5_6_AND_7_8\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_TX_INPUT_FREQ_SYNC_LANES_MAY_BE_ASYNCHRONOUS_IN_FREQUENCY:
		printf("\tLANES_MAY_BE_ASYNCHRONOUS_IN_FREQUENCY\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: tx_input_freq_sync=%u is invalid",
			     module_info_struct->tx_input_freq_sync);
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("cable_attenuation_53g: %u\n", module_info_struct->cable_attenuation_53g);
	printf("rx_output_valid: %u\n", module_info_struct->rx_output_valid);
	printf("max_fiber_length: %u\n", module_info_struct->max_fiber_length);
	printf("error_code (%u):", module_info_struct->error_code);
	switch (module_info_struct->error_code) {
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGUNDEFINED:
		printf("\tCONFIG_UNDEFINED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGSUCCESS:
		printf("\tCONFIG_SUCCESS\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGREJECTED:
		printf("\tCONFIG_REJECTED\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGREJECTEDINVALIDAPPSEL:
		printf("\tCONFIG_REJECTED_INVALID_APPSEL\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGREJECTEDINVALIDDATAPATH:
		printf("\tCONFIG_REJECTED_IN_VALID_DATAPATH\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGREJECTEDINVALIDSI:
		printf("\tCONFIG_REJECTED_INVALID_SI\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGREJECTEDLANESINUSE:
		printf("\tCONFIG_REJECTED_LANES_IN_USE\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGREJECTEDPARTIALDATAPATH:
		printf("\tCONFIG_REJECTED_PARTIAL_DATAPATH\n");
		break;
	case DOCA_TELEMETRY_PHY_PDDR_MODULE_INFO_ERROR_CODE_CONFIGINPROGRESS:
		printf("\tCONFIG_IN_PROGRESS\n");
		break;
	default:
		DOCA_LOG_ERR("Failed to print module info: error_code=%u is invalid", module_info_struct->error_code);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Clean sample objects
 *
 * Closes and frees sample resources.
 *
 * @sample_objects [in]: sample objects to clean
 *
 * @return: DOCA_SUCCESS in case of success, DOCA_ERROR otherwise
 */
static doca_error_t telemetry_phy_sample_cleanup(struct telemetry_phy_sample_objects *sample_objects)
{
	doca_error_t result;

	if (sample_objects->operation_info_struct) {
		DOCA_LOG_INFO("operation_info_struct %p: operation_info_struct was destroyed",
			      sample_objects->operation_info_struct);
		free(sample_objects->operation_info_struct);
		sample_objects->operation_info_struct = NULL;
	}

	if (sample_objects->module_info_struct) {
		DOCA_LOG_INFO("module_info_struct %p: module_info_struct was destroyed",
			      sample_objects->module_info_struct);
		free(sample_objects->module_info_struct);
		sample_objects->module_info_struct = NULL;
	}

	if (sample_objects->telemetry_phy_obj != NULL) {
		result = doca_telemetry_phy_stop(sample_objects->telemetry_phy_obj);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop telemetry_phy with error=%s", doca_error_get_name(result));
			return result;
		}

		result = doca_telemetry_phy_destroy(sample_objects->telemetry_phy_obj);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy telemetry_phy with error=%s", doca_error_get_name(result));
			return result;
		}
		sample_objects->telemetry_phy_obj = NULL;
	}

	if (sample_objects->dev != NULL) {
		result = doca_dev_close(sample_objects->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to close device with error=%s", doca_error_get_name(result));
			return result;
		}
		sample_objects->dev = NULL;
	}

	return DOCA_SUCCESS;
}

/*
 * Allocate telemetry dpa output object
 *
 * @cfg [in]: configuration parameters
 * @sample_objects [in]: sample objects struct for the sample
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t doca_telemetry_phy_sample_allocate_output_objects(
	const struct telemetry_phy_sample_cfg *cfg,
	struct telemetry_phy_sample_objects *sample_objects)
{
	if (cfg->get_operation_info) {
		sample_objects->operation_info_struct = (struct doca_telemetry_phy_pddr_operation_info *)malloc(
			sizeof(struct doca_telemetry_phy_pddr_operation_info));
		if (sample_objects->operation_info_struct == NULL) {
			DOCA_LOG_ERR("Failed to allocate output objects: failed to allocate memory for operation info");
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	if (cfg->get_module_info) {
		sample_objects->module_info_struct = (struct doca_telemetry_phy_pddr_module_info *)malloc(
			sizeof(struct doca_telemetry_phy_pddr_module_info));
		if (sample_objects->module_info_struct == NULL) {
			DOCA_LOG_ERR("Failed to allocate output objects: failed to allocate memory for module info");
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize telemetry phy context object
 *
 * @cfg [in]: configuration parameters
 * @sample_objects [in]: sample objects struct for the sample
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
static doca_error_t telemetry_phy_sample_context_init(const struct telemetry_phy_sample_cfg *cfg,
						      struct telemetry_phy_sample_objects *sample_objects)
{
	doca_error_t result;

	struct doca_devinfo *devinfo = doca_dev_as_devinfo(sample_objects->dev);

	result = doca_telemetry_phy_cap_is_supported(devinfo);
	if (result == DOCA_ERROR_NOT_SUPPORTED) {
		DOCA_LOG_ERR("Failed to start telemetry_phy: device does not support doca_telemetry_phy");
		return DOCA_ERROR_NOT_SUPPORTED;
	} else if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_phy: failed to query capability");
		return result;
	}

	if (cfg->get_operation_info) {
		result = doca_telemetry_phy_cap_operation_info_is_supported(devinfo);
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_ERR(
				"Failed to start telemetry_phy: device does not support doca_telemetry_phy operation info");
			return DOCA_ERROR_NOT_SUPPORTED;
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start telemetry_phy: failed to query capability for operation info");
			return result;
		}
	}

	if (cfg->get_module_info) {
		result = doca_telemetry_phy_cap_module_info_is_supported(devinfo);
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_ERR(
				"Failed to start telemetry_phy: device does not support doca_telemetry_phy module info");
			return DOCA_ERROR_NOT_SUPPORTED;
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start telemetry_phy: failed to query capability for module info");
			return result;
		}
	}

	/* Create context and set properties */
	result = doca_telemetry_phy_create(sample_objects->dev, &sample_objects->telemetry_phy_obj);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_phy: failed to create telemetry phy object with error=%s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_phy_sample_allocate_output_objects(cfg, sample_objects);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry_dpa: failed to init sample objects with error=%s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_telemetry_phy_start(sample_objects->telemetry_phy_obj);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start telemetry phy object with error=%s", doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t telemetry_phy_sample_run(const struct telemetry_phy_sample_cfg *cfg)
{
	doca_error_t result = DOCA_SUCCESS, teardown_result = DOCA_SUCCESS;
	struct telemetry_phy_sample_objects sample_objects = {0};

	DOCA_LOG_INFO("Started doca_telemetry_phy sample with the following parameters: ");
	DOCA_LOG_INFO("	pci_addr='%s'", cfg->pci_addr);
	if (cfg->get_operation_info) {
		DOCA_LOG_INFO("	Retrieve operation info");
	}
	if (cfg->get_module_info) {
		DOCA_LOG_INFO("	Retrieve module info");
	}

	/* Open DOCA device based on the given PCI address */
	result = open_doca_device_with_pci(cfg->pci_addr, NULL, &sample_objects.dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open device with error=%s", doca_error_get_name(result));
		return result;
	}

	result = telemetry_phy_sample_context_init(cfg, &sample_objects);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init sample objects with error=%s", doca_error_get_name(result));
		goto teardown;
	}

	if (cfg->get_operation_info) {
		result = doca_telemetry_phy_get_operation_info(sample_objects.telemetry_phy_obj,
							       sample_objects.operation_info_struct);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to read operation info with error=%s", doca_error_get_name(result));
			goto teardown;
		}

		result = telemetry_phy_print_operation_info(sample_objects.operation_info_struct);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to print operation info with error=%s", doca_error_get_name(result));
			goto teardown;
		}
	}

	if (cfg->get_module_info) {
		result = doca_telemetry_phy_get_module_info(sample_objects.telemetry_phy_obj,
							    sample_objects.module_info_struct);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to read module info with error=%s", doca_error_get_name(result));
			goto teardown;
		}

		result = telemetry_phy_print_module_info(sample_objects.module_info_struct);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to print module info with error=%s", doca_error_get_name(result));
			goto teardown;
		}
	}

teardown:
	teardown_result = telemetry_phy_sample_cleanup(&sample_objects);
	if (teardown_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Teardown failed with error=%s", doca_error_get_name(teardown_result));
		DOCA_ERROR_PROPAGATE(result, teardown_result);
	}

	return result;
}
