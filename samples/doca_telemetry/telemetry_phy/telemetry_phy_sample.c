/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#define TIME_CONVERT_MS_MIN 60000 // Value required to convert time to mins as provided as ms

struct telemetry_phy_sample_objects {
	struct doca_telemetry_phy *telemetry_phy_obj;			 /* doca telemetry phy object*/
	struct doca_dev *dev;						 /* Doca device*/
	struct doca_telemetry_phy_operation_info *operation_info_struct; /* Structure that represent the operation info
									  */
	struct doca_telemetry_phy_module_info *module_info_struct;	 /* Structure that represent the module info */
	struct doca_telemetry_phy_counter_and_ber_info *counter_and_ber_info_struct; /* Structure that
												 represent the physical
												 counter and BER info */
};

/*
 * Print operation info
 *
 * Print the contents of the extracted operation info.
 *
 * @operation_info_struct [in]: Extracted operation_info_struct to print
 */
static void telemetry_phy_print_operation_info(struct doca_telemetry_phy_operation_info *operation_info_struct)
{
	printf("\nOperational info\n");
	printf("----------------\n");

	printf("Active protocol: ");
	switch (operation_info_struct->active_protocol) {
	case DOCA_TELEMETRY_PHY_PROTOCOL_IB:
		printf("INFINIBAND\n");
		break;
	case DOCA_TELEMETRY_PHY_PROTOCOL_ETH:
		printf("ETHERNET\n");
		break;
	default:
		printf("N/A\n");
	}

	printf("State: ");
	switch (operation_info_struct->state) {
	case DOCA_TELEMETRY_PHY_STATE_DISABLED:
		printf("DISABLED\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_OPEN_PORT:
		printf("OPEN PORT\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_POLLING:
		printf("POLLING\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_ACTIVE:
		printf("ACTIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_CLOSE_PORT:
		printf("CLOSE PORT\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_PHY_UP:
		printf("PHY UP\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_SLEEP:
		printf("SLEEP\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_SIGNAL_DETECT:
		printf("SIGNAL DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_RCVR_DETECT:
		printf("RECEIVER DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_SYNC_PEER:
		printf("SYNC PEER\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_NEGOTIATION:
		printf("NEGOTIATION\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_TRAINING:
		printf("TRAINING\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_SUBFSM_ACTIVE:
		printf("SUBFSM ACTIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_PROTOCOL_DETECT:
		printf("PROTOCOL DETECT\n");
		break;
	case DOCA_TELEMETRY_PHY_STATE_UNKNOWN:
		printf("UNKNOWN\n");
		break;
	default:
		printf("N/A\n");
	}

	printf("Physical state: ");
	switch (operation_info_struct->active_protocol) {
	case DOCA_TELEMETRY_PHY_PROTOCOL_ETH: {
		switch (operation_info_struct->phy_state.eth_phy_state) {
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_ENABLE:
			printf("ENABLE\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_XMIT_DISABLE:
			printf("XMIT DISABLE\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_ABILITY_DETECT:
			printf("ABILITY DETECT\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_ACK_DETECT:
			printf("ACK DETECT\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_COMPLETE_ACK:
			printf("COMPLETE ACK\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_AN_GOOD_CHECK:
			printf("AN GOOD CHECK\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_LINK_UP:
			printf("LINK UP\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_PHY_STATE_NEXT_PAGE_WAIT:
			printf("NEXT PAGE WAIT\n");
			break;
		default:
			printf("N/A\n");
		}
	} break;
	case DOCA_TELEMETRY_PHY_PROTOCOL_IB: {
		switch (operation_info_struct->phy_state.ib_phy_state) {
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_DISABLED:
			printf("DISABLED\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_INITIALIZING:
			printf("INITIALIZING\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_RECOVER_CFG:
			printf("RECOVER CONFIG\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_CFG_TEST:
			printf("CONFIG TEST\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_WAIT_REMOTE_TEST:
			printf("WAIT REMOTE TEST\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_WAIT_CFG_ENHANCED:
			printf("WAIT CONFIG ENHANCED\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_CFG_IDLE:
			printf("CONFIG IDLE\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_LINK_UP:
			printf("LINK UP\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_PHY_STATE_AN_FSM_POLLING:
			printf("POLLING\n");
			break;
		default:
			printf("N/A\n");
		}
	} break;
	default:
		printf(": \tN/A\n");
	}

	printf("Speed: ");
	switch (operation_info_struct->active_protocol) {
	case DOCA_TELEMETRY_PHY_PROTOCOL_ETH: {
		switch (operation_info_struct->link_speed_active.link_speed_eth) {
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_UNKNOWN:
			printf("UNKNOWN\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_10M:
			printf("10M\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_10M_BASE_T:
			printf("10M_BASE_T\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_100M:
			printf("100M\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_100M_BASE_TX:
			printf("100M_BASE_TX\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_1000M_BASE_T:
			printf("1000M_BASE_T\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_CX:
			printf("CX\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_KX:
			printf("KX\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_CX4:
			printf("CX4\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_KX4:
			printf("KX4\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_1G:
			printf("1G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_5G:
			printf("5G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_10G_BASE_T:
			printf("10G_BASE_T\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_10G:
			printf("10G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_25G:
			printf("25G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_40G:
			printf("40G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_50G:
			printf("50G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_100G:
			printf("100G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_200G:
			printf("200G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_400G:
			printf("400G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_800G:
			printf("800G\n");
			break;
		case DOCA_TELEMETRY_PHY_ETH_LINK_SPEED_1600G:
			printf("1600G\n");
			break;
		default:
			printf("N/A\n");
		}
	} break;
	case DOCA_TELEMETRY_PHY_PROTOCOL_IB: {
		switch (operation_info_struct->link_speed_active.link_speed_ib) {
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_SDR:
			printf("SDR\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_DDR:
			printf("DDR\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_QDR:
			printf("QDR\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_FDR10:
			printf("FDR10\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_FDR:
			printf("FDR\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_EDR:
			printf("EDR\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_HDR:
			printf("HDR\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_NDR:
			printf("NDR\n");
			break;
		case DOCA_TELEMETRY_PHY_IB_LINK_SPEED_XDR:
			printf("XDR\n");
			break;
		default:
			printf("N/A\n");
		}
	} break;
	default:
		printf(": \tN/A\n");
	}

	printf("Link width: %u\n", operation_info_struct->link_width);

	printf("FEC mode active: ");
	switch (operation_info_struct->fec_mode_active) {
	case DOCA_TELEMETRY_PHY_NO_FEC:
		printf("NO FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_FIRECODE_FEC:
		printf("FIRECODE FEC\n");
		break;
	case DOCA_TELEMETRY_PHY_STANDARD_RS_FEC_528_514:
		printf("STANDARD RS_FEC (528,514)\n");
		break;
	case DOCA_TELEMETRY_PHY_STANDARD_LL_RS_FEC_271_257:
		printf("STANDARD LL RS_FEC (271,257)\n");
		break;
	case DOCA_TELEMETRY_PHY_INTERLEAVED_QUAD_RS_FEC_544_514:
		printf("INTERLEAVED QUAD RS_FEC (544,514)\n");
		break;
	case DOCA_TELEMETRY_PHY_INTERLEAVED_QUAD_RS_FEC_PLR_546_516:
		printf("INTERLEAVED QUAD RS_FEC + PLR (546,516)\n");
		break;
	case DOCA_TELEMETRY_PHY_INTERLEAVED_STANDARD_RS_544_514:
		printf("INTERLEAVED STANDARD RS (544,514)\n");
		break;
	case DOCA_TELEMETRY_PHY_STANDARD_RS_FEC_544_514:
		printf("STANDARD RS_FEC (544,514)\n");
		break;
	case DOCA_TELEMETRY_PHY_INTERLEAVED_OCTET_RS_FEC_PLR_546_516:
		printf("INTERLEAVED OCTET RS_FEC + PLR (546,516)\n");
		break;
	case DOCA_TELEMETRY_PHY_ETH_CONSORTIUM_LL_50G_RS_FEC_272_257_PLUS_1:
		printf("ETH CONSORTIUM LL 50G RS FEC (272,257+1)\n");
		break;
	case DOCA_TELEMETRY_PHY_INTERLEAVED_ETH_CONSORTIUM_LL_50G_RS_FEC_272_257_PLUS_1:
		printf("INTERLEAVED ETH CONSORTIUM LL 50G RS_FEC (272,257+1)\n");
		break;
	case DOCA_TELEMETRY_PHY_INTERLEAVED_STANDARD_RS_FEC_PLR:
		printf("INTERLEAVED STANDARD RS_FEC + PLR\n");
		break;
	default:
		printf("N/A\n");
	}

	printf("Loopback mode: ");
	switch (operation_info_struct->loopback_mode) {
	case DOCA_TELEMETRY_PHY_NO_LOOPBACK_ACTIVE:
		printf("NO LOOPBACK ACTIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_REMOTE_LOOPBACK:
		printf("PHY REMOTE LOOPBACK\n");
		break;
	case DOCA_TELEMETRY_PHY_LOCAL_LOOPBACK:
		printf("PHY LOCAL LOOPBACK\n");
		break;
	case DOCA_TELEMETRY_PHY_EXTERNAL_LOCAL_LOOPBACK:
		printf("EXTERNAL LOCAL LOOPBACK\n");
		break;
	default:
		printf("N/A\n");
	}

	printf("Auto Negotiation: ");
	switch (operation_info_struct->auto_negotiation) {
	case DOCA_TELEMETRY_PHY_AUTO_NEGOTIATION_ON:
		printf("ON\n");
		break;
	case DOCA_TELEMETRY_PHY_AUTO_NEGOTIATION_FORCE:
		printf("FORCE\n");
		break;
	default:
		printf("N/A\n");
	}
	printf("\n");
}

/*
 * Print QSFP CMIS cable technology info
 *
 * Print the contents of the extracted Module info QSFP CMIS cable technology.
 *
 * @QSFP_CMIS_cable_technology [in]: Extracted QSFP CMIS cable technology to print
 */
static void telemetry_phy_print_module_info_QSFP_CMIS_cable_technology(
	enum doca_telemetry_phy_QSFP_CMIS_cable_technology QSFP_CMIS_cable_technology)
{
	switch (QSFP_CMIS_cable_technology) {
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_VCSEL_850NM:
		printf("VCSEL 850nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_VCSEL_1310NM:
		printf("VCSEL 1310nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_VCSEL_1550NM:
		printf("VCSEL 1550nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_FP_LASER_1310NM:
		printf("FP LASER 1310nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_DFB_LASER_1310NM:
		printf("DFB LASER 1310nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_DFB_LASER_1550NM:
		printf("DFB LASER 15500nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_EML_1310NM:
		printf("EML 1310nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_EML_1550NM:
		printf("EML 1550nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_OTHERS:
		printf("Others\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_DFB_LASER_1490NM:
		printf("DFB LASER 1490nm\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_PASSIVE_COPPER_CABLE_UNEQD:
		printf("Passive Copper Cable Unequalized\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_PASSIVE_COPPER_CABLE_EQD:
		printf("Passive Copper Cable Equalized\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_COPPER_CABLE_NEAR_END_AND_FAR_END_LIMITING_ACT_EQ:
		printf("Copper Cable Near End And Far End Limiting Active Equalizer\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_COPPER_CABLE_FAR_END_LIMITING_ACT_EQ:
		printf("Copper Cable Far End Limiting Active Equalizer\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_COPPER_CABLE_NEAR_END_LIMITING_ACT_EQ:
		printf("Copper Cable Near End Limiting Active Equalizer\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_COPPER_CABLE_LINEAR_ACT_EQS:
		printf("Copper Cable Linear Active Equalizers\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_C_BAND_TUNABLE_LASER:
		printf("C Band Tunable Laser\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_L_BAND_TUNABLE_LASER:
		printf("L Band Tunable Laser\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_COPPER_CABLE_NEAR_END_AND_FAR_END_LINEAR_ACT_EQS:
		printf("Copper Cable Near End And Far End Linear Active Equalizers\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_COPPER_CABLE_FAR_END_LINEAR_ACT_EQS:
		printf("Copper Cable Far End Linear Active Equalizers\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_COPPER_CABLE_NEAR_END_LINEAR_ACT_EQS:
		printf("Copper Cable Near End Linear Active Equalizers\n");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_CMIS_NA:
	default:
		printf("N/A\n");
	}
}

/*
 * Print SFP cable technology info
 *
 * Print the contents of the extracted Module info SFP cable technology.
 *
 * @QSFP_CMIS_cable_technology [in]: Extracted SFP cable technology to print
 */
static void telemetry_phy_print_module_info_SFP_cable_technology(
	enum doca_telemetry_phy_SFP_cable_technology SFP_cable_technology)
{
	switch (SFP_cable_technology) {
	case DOCA_TELEMETRY_PHY_SFP_PASSIVE:
		printf("PASSIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_SFP_ACTIVE:
		printf("ACTIVE\n");
		break;
	case DOCA_TELEMETRY_PHY_SFP_NA:
	default:
		printf("N/A\n");
	}
}

/*
 * Print Cable Type info
 *
 * Print the contents of the extracted Module info Cable Type.
 *
 * @cable_type [in]: Extracted Cable Type to print
 */
static void telemetry_phy_print_module_info_cable_type(enum doca_telemetry_phy_cable_type cable_type)
{
	printf("Cable Type: ");
	switch (cable_type) {
	case DOCA_TELEMETRY_PHY_CABLE_TYPE_UNIDENTIFIED:
		printf("Unidentified\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_TYPE_ACTIVE_CABLE:
		printf("Active cable (active copper / optics)\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_TYPE_OPTICAL_MODULE:
		printf("Optical Module (separated)\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_TYPE_PASSIVE_COPPER_CABLE_OR_LINEAR_COPPER:
		printf("Passive copper cable or Linear copper\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_TYPE_CABLE_UNPLUGGED:
		printf("Cable unplugged\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_TYPE_TWISTED_PAIR:
		printf("Twisted Pair\n");
		break;
	default:
		printf("Unidentified\n");
	}
}

/*
 * Print Cable Vendor info
 *
 * Print the contents of the extracted Module info Cable Vendor.
 *
 * @cable_vendor [in]: Extracted Cable Vendor to print
 */
static void telemetry_phy_print_module_info_cable_vendor(enum doca_telemetry_phy_cable_vendor cable_vendor)
{
	printf("OUI: ");
	switch (cable_vendor) {
	case DOCA_TELEMETRY_PHY_CABLE_VENDOR_OTHER:
		printf("Other\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_VENDOR_MELLANOX:
		printf("Mellanox\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_VENDOR_KNOWN_OUI:
		printf("Known OUI\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_VENDOR_NVIDIA:
		printf("Nvidia\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_VENDOR_NA:
		printf("N/A\n");
		break;
	default:
		printf("Unknown\n");
	}
}

/*
 * Print string of QSFP Compliance Code
 *
 * @qsfp_cc [in]: QSFP Compliance Code to print
 */
static void telemetry_phy_print_QSFP_cc(uint8_t qsfp_cc)
{
	int text_printed = 0;

	printf("[QSFP] ");
	if (qsfp_cc & DOCA_TELEMETRY_PHY_QSFP_CC_40G_ACTIVE_CABLE_XLPPI) {
		printf("40G Active Cable (XLPPI)");
		text_printed = 1;
	}

	if (qsfp_cc & DOCA_TELEMETRY_PHY_QSFP_CC_40GBASE_LR4) {
		if (text_printed) {
			printf(",");
		}
		printf("40GBASE-LR4");
		text_printed = 1;
	}

	if (qsfp_cc & DOCA_TELEMETRY_PHY_QSFP_CC_40GBASE_SR4) {
		if (text_printed) {
			printf(",");
		}
		printf("40GBASE-SR4");
		text_printed = 1;
	}

	if (qsfp_cc & DOCA_TELEMETRY_PHY_QSFP_CC_40GBASE_CR4) {
		if (text_printed) {
			printf(",");
		}
		printf("40GBASE-CR4");
		text_printed = 1;
	}

	if (qsfp_cc & DOCA_TELEMETRY_PHY_QSFP_CC_10GBASE_SR) {
		if (text_printed) {
			printf(",");
		}
		printf("10GBASE-SR");
		text_printed = 1;
	}

	if (qsfp_cc & DOCA_TELEMETRY_PHY_QSFP_CC_10GBASE_LR) {
		if (text_printed) {
			printf(",");
		}
		printf("10GBASE-LR");
		text_printed = 1;
	}

	if (qsfp_cc & DOCA_TELEMETRY_PHY_QSFP_CC_10GBASE_LRM) {
		if (text_printed) {
			printf(",");
		}
		printf("10GBASE-LRM");
		text_printed = 1;
	}

	if (!text_printed) {
		printf("N/A");
	}
}

/*
 * Print string of SFP Compliance Code
 *
 * @sfp_cc [in]: SFP Compliance Code to print
 */
static void telemetry_phy_print_SFP_cc(uint8_t sfp_cc)
{
	int text_printed = 0;

	printf("[SFP] ");
	if (sfp_cc & DOCA_TELEMETRY_PHY_SFP_CC_10G_BASE_SR) {
		printf("10GBASE-SR");
		text_printed = 1;
	}

	if (sfp_cc & DOCA_TELEMETRY_PHY_SFP_CC_10G_BASE_LR) {
		if (text_printed) {
			printf(",");
		}
		printf("10GBASE-LR");
		text_printed = 1;
	}

	if (sfp_cc & DOCA_TELEMETRY_PHY_SFP_CC_10G_BASE_LRM) {
		if (text_printed) {
			printf(",");
		}
		printf("10GBASE-LRM");
		text_printed = 1;
	}

	if (sfp_cc & DOCA_TELEMETRY_PHY_SFP_CC_10G_BASE_ER) {
		if (text_printed) {
			printf(",");
		}
		printf("10GBASE-ER");
		text_printed = 1;
	}

	if (!text_printed) {
		printf("N/A");
	}
}

/*
 * Print string of QSFP SFP Common Compliance Code
 *
 * @QSFP_SFP_common_cc [in]: QSFP SFP Common Compliance Code to print
 */
static void telemetry_phy_print_QSFP_SFP_common_cc(enum doca_telemetry_phy_QSFP_SFP_common_cc QSFP_SFP_common_cc)
{
	printf("[QSFP/SFP Common] ");
	switch (QSFP_SFP_common_cc) {
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_UNSPECIFIED:
		printf("N/A");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_AOC_OR_25GAUI_C2M_AOC_WITH_FEC:
		printf("100G AOC (Active Optical Cable) or 25GAUI C2M AOC with FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_SR4_OR_25GBASE_SR:
		printf("100GBASE-SR4 or 25GBASE-SR");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_LR4_OR_25GBASE_LR:
		printf("100GBASE-LR4 or 25GBASE-LR");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_ER4_OR_25GBASE_ER:
		printf("100GBASE-ER4 or 25GBASE-ER");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_SR10:
		printf("100GBASE-SR10");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_CWDM4:
		printf("100G CWDM4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_PSM4_PARALLEL_SMF:
		printf("100G PSM4 Parallel SMF");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_ACC_OR_25GAUI_C2M_ACC_WITH_FEC:
		printf("100G ACC (Active Copper Cable) or 25GAUI C2M ACC. with FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_CR4_OR_25GBASE_CR_CA_25G_L_OR_50GBASE_CR2_WITH_RS_CLAUSE91_FEC:
		printf("100GBASE-CR4, 25GBASE-CR CA-25G-L or 50GBASE-CR2 with RS (Clause91) FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_25GBASE_CR_CA_S_OR_50GBASE_CR2_WITH_BASE_R_CLAUSE_74_FIRE_CODE_FEC:
		printf("25GBASE-CR CA-25G-S or 50GBASE-CR2 with BASE-R (Clause 74 Fire code) FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_25GBASE_CR_CA_N_OR_50GBASE_CR2_WITH_NO_FEC:
		printf("25GBASE-CR CA-25G-N or 50GBASE-CR2 with no FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_40GBASE_ER4:
		printf("40GBASE-ER4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_4_X_10GBASE_SR:
		printf("4 x 10GBASE-SR");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_40G_PSM4_PARALLEL_SMF:
		printf("40G PSM4 Parallel SMF");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_G959_1_P1I1_2D1_10709_MBD_2KM_1310NM_SM:
		printf("G959.1 profile P1I1-2D1 (10709 MBd, 2km, 1310nm SM)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_G959_1_P1S1_2D2_10709_MBD_40KM_1550NM_SM:
		printf("G959.1 profile P1S1-2D2 (10709 MBd, 40km, 1550nm SM)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_G959_1_P1L1_2D2_10709_MBD_80KM_1550NM_SM:
		printf("G959.1 profile P1L1-2D2 (10709 MBd, 80km, 1550nm SM)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_10GBASE_T_WITH_SFI_ELECTRICAL_INTERFACE:
		printf("10GBASE-T with SFI electrical interface");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_CLR4:
		printf("100G CLR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_AOC_OR_25GAUI_C2M_AOC_WITH_NO_FEC:
		printf("100G AOC or 25GAUI C2M AOC. No FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_ACC_OR_25GAUI_C2M_ACC_WITH_NO_FEC:
		printf("100G ACC or 25GAUI C2M ACC. No FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_DWDM2:
		printf("100GE-DWDM2 (DWDM transceiver using 2 wavelengths on a 1550 nm DWDM grid with a reach up to 80 km)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_1550NM_WDM:
		printf("100G 1550nm WDM (4 wavelengths)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_10GBASE_T_2:
		printf("10GBASE-T Short Reach (30 meters)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_5GBASE_T:
		printf("5GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_2_5GBASE_T:
		printf("2.5GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_40G_SWDM4:
		printf("40G SWDM4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_SWDM4:
		printf("100G SWDM4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_PAM4_BIDI:
		printf("100G PAM4 BiDi");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_4WDM10_MSA:
		printf("4WDM-10 MSA (10km version of 100G CWDM4 with same RS(528,514) FEC in host system)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_4WDM20_MSA:
		printf("4WDM-10 MSA (20km version of 100G CWDM4 with same RS(528,514) FEC in host system)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_4WDM40_MSA:
		printf("4WDM-10 MSA (40km version of 100G CWDM4 with same RS(528,514) FEC in host system)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_DR_WITH_CAUI_4_WITHOUT_FEC:
		printf("100GBASE-DR, with CAUI-4 without FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_FR_WITH_CAUI_4_WITHOUT_FEC:
		printf("100G-FR, with CAUI-4 without FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100G_LR_WITH_CAUI_4_WITHOUT_FEC:
		printf("100G-LR, with CAUI-4 without FEC");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_SR1_OR_200GBASE_SR2_OR_400GBASE_SR4:
		printf("100GBASE-SR1, 200GBASE-SR2 or 400GBASE-SR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_FR1_OR_400GBASE_DR4_2:
		printf("100GBASE-FR1 or 400GBASE-DR4-2");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_LR1:
		printf("100GBASE-LR1");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_ACC_10_6_WITH_50GAUI_OR_100GAUI_2_OR_200GAUI_4_C2M:
		printf("Active Copper Cable with 50GAUI, 100GAUI-2 or 200GAUI-4 C2M. Providing a worst BER of 10^(-6) or below");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_AOC_10_6_WITH_50GAUI_OR_100GAUI_2_OR_200GAUI_4_C2M:
		printf("Active Optical Cable with 50GAUI, 100GAUI-2 or 200GAUI-4 C2M. Providing a worst BER of 10^(-6) or below");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_ACC_10_4_WITH_50GAUI_OR_100GAUI_2_OR_200GAUI_4_C2M:
		printf("Active Copper Cable with 50GAUI, 100GAUI-2 or 200GAUI-4 C2M. Providing a worst BER of 2.6x10^(-4) for ACC, 10^(-5) for AUI, or below");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_AOC_10_4_WITH_50GAUI_OR_100GAUI_2_OR_200GAUI_4_C2M:
		printf("Active Optical Cable with 50GAUI, 100GAUI-2 or 200GAUI-4 C2M. Providing a worst BER of 2.6x10^(-4) for AOC, 10^(-5) for AUI, or below");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_VR1_OR_200GBASE_VR2_OR_400GBASE_VR4:
		printf("100GBASE-VR1, 200GBASE-VR2 or 400GBASE-VR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_100GBASE_CR1_OR_200GBASE_CR2_OR_400GBASE_CR4:
		printf("100GBASE-CR1, 200GBASE-CR2 or 400GBASE-CR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_50GBASE_CR_OR_100GBASE_CR2_OR_200GBASE_CR4:
		printf("50GBASE-CR, 100GBASE-CR2, or 200GBASE-CR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_50GBASE_SR_OR_100GBASE_SR2_OR_200GBASE_SR4:
		printf("50GBASE-SR, 100GBASE-SR2, or 200GBASE-SR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_50GBASE_FR_OR_200GBASE_DR4:
		printf("50GBASE-FR or 200GBASE-DR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_200GBASE_FR4:
		printf("200GBASE-FR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_200GBASE_1550NM_PSM4:
		printf("200G 1550 nm PSM4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_50GBASE_LR:
		printf("150GBASE-LR");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_200GBASE_LR4:
		printf("200GBASE-LR4");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SFP_CC_400GBASE_DR4_OR_400GAUI_4_C2M:
		printf("400GBASE-DR4, 400GAUI-4 C2M");
		break;
	default:
		printf("N/A");
	}
}

/*
 * Print string of CMIS Common Compliance Code
 *
 * @CMIS_common_cc [in]: CMIS Common Compliance Code to print
 */
static void telemetry_phy_print_CMIS_common_cc(enum doca_telemetry_phy_CMIS_common_cc CMIS_common_cc)
{
	printf("[CMIS Common] ");
	switch (CMIS_common_cc) {
	case DOCA_TELEMETRY_PHY_CMIS_CC_UNSPECIFIED:
		printf("N/A");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_1000_BASE_CX:
		printf("1000BASE-CX");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_XAUI:
		printf("XAUI");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_XFI:
		printf("XFI");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_SFI:
		printf("SFI");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_25G_AUI:
		printf("25GAUI");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_XL_AUI:
		printf("XLAUI");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_XL_PPI:
		printf("XLPPI");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_L_AUI2:
		printf("LAUI-2");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_50G_AUI2:
		printf("50GAUI-2");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_50G_AUI1:
		printf("50GAUI-1");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_C_AUI4:
		printf("CAUI-4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_100G_AUI4:
		printf("100GAUI-4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_100G_AUI2:
		printf("100GAUI-2");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_200G_AUI8:
		printf("200GAUI-8");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_200G_AUI4:
		printf("200GAUI-4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_400G_AUI16:
		printf("400GAUI-16");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_400G_AUI8:
		printf("400GAUI-8");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_10G_BASE_CX4:
		printf("10GBASE-CX4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_25G_CR_L:
		printf("25GBASE-CR CA-L");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_25G_CR_S:
		printf("25GBASE-CR CA-S");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_25G_CR_N:
		printf("25GBASE-CR CA-N");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_40G_BASE_CR4:
		printf("40GBASE-CR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_50G_BASE_CR:
		printf("50GBASE-CR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_100G_BASE_CR10:
		printf("100GBASE-CR10");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_100G_BASE_CR4:
		printf("100GBASE-CR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_100G_BASE_CR2:
		printf("100GBASE-CR2");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_200G_BASE_CR4:
		printf("200GBASE-CR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_400G_CR8:
		printf("400G CR8");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_1000_BASE_T:
		printf("1000BASE-T");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_2_5G_BASE_T:
		printf("2.5GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_5G_BASE_T:
		printf("5GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_10G_BASE_T:
		printf("10GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_25_BASE_T:
		printf("25GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_40_BASE_T:
		printf("40GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_50_BASE_T:
		printf("50GBASE-T");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_SDR:
		printf("IB SDR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_FDR:
		printf("IB FDR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_EDR:
		printf("IB EDR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_HDR:
		printf("IB HDR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_NDR:
		printf("IB NDR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_XDR:
		printf("IB XDR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_100G_BASE_R1:
		printf("100GBASE-R1");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_200G_BASE_R2:
		printf("200GBASE-R2");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_400G_BASE_R4:
		printf("400GBASE-R4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CC_800G_ETC_CR8_OR_800GBASE_CR8:
		printf("800G-ETC-CR8 or 800GBASE-CR8");
		break;
	default:
		printf("N/A");
	}
}

/*
 * Print CMIS Copper Compliance Code
 *
 * @CMIS_copper_cc [in]: CMIS Copper Compliance Code to print
 */
static void telemetry_phy_print_CMIS_copper_cc(enum doca_telemetry_phy_CMIS_copper CMIS_copper_cc)
{
	printf("[Copper] ");
	switch (CMIS_copper_cc) {
	case DOCA_TELEMETRY_PHY_CMIS_COPPER_CC_UNSPECIFIED:
		printf("N/A");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_COPPER_CC_ASSEMBLY_BER_LT_1E_12:
		printf("Active Cable assembly with BER < 10-12");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_COPPER_CC_ASSEMBLY_BER_LT_5E_5:
		printf("Active Cable assembly with BER < 5x10-5");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_COPPER_CC_ASSEMBLY_BER_LT_2_6E_4:
		printf("Active Cable assembly with BER < 2.6x10-4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_COPPER_CC_ASSEMBLY_BER_LT_1E_6:
		printf("Active Cable assembly with BER < 10-6");
		break;
	default:
		printf("N/A");
	}
}

/*
 * Print CMIS Optical Single Mode Compliance Code
 *
 * @CMIS_optical_sm_cc [in]: CMIS Optical Single Mode Fiber Compliance Code to print
 */
static void telemetry_phy_print_CMIS_optical_sm_cc(enum doca_telemetry_phy_CMIS_optical_sm CMIS_optical_sm_cc)
{
	printf("[Single Mode Fiber] ");
	switch (CMIS_optical_sm_cc) {
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_UNSPECIFIED:
		printf("N/A");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_BASE_LW:
		printf("10GBASE-LW");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_BASE_EW:
		printf("10GBASE-EW");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_ZW:
		printf("10G-ZW");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_BASE_LR:
		printf("10GBASE-LR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_BASE_ER:
		printf("10GBASE-ER");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_BASE_ZR:
		printf("10G-ZR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_25G_BASE_LR:
		printf("25GBASE-LR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_25G_BASE_ER:
		printf("25GBASE-ER");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_40G_BASE_LR4:
		printf("40GBASE-LR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_40G_BASE_FR:
		printf("40GBASE-FR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_50G_BASE_FR:
		printf("50GBASE-FR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_50G_BASE_LR:
		printf("50GBASE-LR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_BASE_LR4:
		printf("100GBASE-LR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_BASE_ER4:
		printf("100GBASE-ER4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_PSM4:
		printf("100G PSM4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_CWDM4_OCP:
		printf("100G CWDM4-OCP");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_CWDM4:
		printf("100G CWDM4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_4WDM_10:
		printf("100G 4WDM-10");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_4WDM_20:
		printf("100G 4WDM-20");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_4WDM_40:
		printf("100G 4WDM-40");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_BASE_DR:
		printf("100GBASE-DR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_FR:
		printf("100G-FR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_100G_LR:
		printf("100G-LR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_200G_BASE_DR4:
		printf("200GBASE-DR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_200G_BASE_FR4:
		printf("200GBASE-FR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_200G_BASE_LR4:
		printf("200GBASE-LR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_400G_BASE_FR8:
		printf("400GBASE-FR8");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_400G_BASE_LR8:
		printf("400GBASE-LR8");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_400G_BASE_DR4:
		printf("400GBASE-DR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_400G_FR4:
		printf("400G-FR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_400G_LR4:
		printf("400G-LR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_SR:
		printf("10G-SR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_LR:
		printf("10G-LR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_25G_SR:
		printf("25G-SR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_25G_LR:
		printf("25G-LR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_10G_LR_BIDI:
		printf("10G-LR-BiDi");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_SM_CC_25G_LR_BIDI:
		printf("25G-LR-BiDi");
		break;
	default:
		printf("N/A");
	}
}

/*
 * Print CMIS Optical Multi Mode Compliance Code
 *
 * @CMIS_optical_mm_cc [in]: CMIS Optical Multi Mode Fiber Compliance Code to print
 */
static void telemetry_phy_print_CMIS_optical_mm_cc(enum doca_telemetry_phy_CMIS_optical_mm CMIS_optical_mm_cc)
{
	printf("[Multi Mode Fiber] ");
	switch (CMIS_optical_mm_cc) {
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_UNSPECIFIED:
		printf("N/A");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_10G_BASE_SW:
		printf("10GBASE-SW");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_10G_BASE_SR:
		printf("10GBASE-SR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_25G_BASE_SR:
		printf("25GBASE-SR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_40G_BASE_SR4:
		printf("40GBASE-SR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_40G_SWDM4:
		printf("40GE SWDM4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_40G_BIDI:
		printf("40GE BiDi");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_50G_BASE_SR:
		printf("50GBASE-SR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_100G_BASE_SR10:
		printf("100GBASE-SR10");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_100G_BASE_SR4:
		printf("100GBASE-SR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_100G_SWDM4:
		printf("100GE SWDM4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_100G_BIDI:
		printf("100GE BiDi");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_100G_SR2:
		printf("100GBASE-SR2");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_100G_SR:
		printf("100G-SR");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_200G_BASE_SR4:
		printf("200GBASE-SR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_400G_BASE_SR16:
		printf("400GBASE-SR16");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_400G_BASE_SR8:
		printf("400G-SR8");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_400G_SR4:
		printf("400G-SR4");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_800G_SR8:
		printf("800G-SR8");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_MM_CC_400G_BIDI:
		printf("400GE BiDi");
		break;
	default:
		printf("N/A");
	}
}

/*
 * Print Ethernet QSFP Compliance Code
 *
 * @module_info_struct [in]: Extracted Module info to print
 */
static void telemetry_phy_print_module_info_eth_QSFP_compliance_code(
	struct doca_telemetry_phy_module_info *module_info_struct)
{
	uint8_t qsfp_cc =
		module_info_struct->cable_general_properties_info.compliance_code.qsfp_sfp_cc.specific_cc.qsfp_cc;
	enum doca_telemetry_phy_QSFP_SFP_common_cc common_cc =
		module_info_struct->cable_general_properties_info.compliance_code.qsfp_sfp_cc.common_cc;

	printf("Compliance: ");
	telemetry_phy_print_QSFP_cc(qsfp_cc);
	telemetry_phy_print_QSFP_SFP_common_cc(common_cc);
	printf("\n");
}

/*
 * Print Ethernet SFP Compliance Code
 *
 * @module_info_struct [in]: Extracted Module info to print
 */
static void telemetry_phy_print_module_info_eth_SFP_compliance_code(
	struct doca_telemetry_phy_module_info *module_info_struct)
{
	uint8_t sfp_cc =
		module_info_struct->cable_general_properties_info.compliance_code.qsfp_sfp_cc.specific_cc.sfp_cc;
	enum doca_telemetry_phy_QSFP_SFP_common_cc common_cc =
		module_info_struct->cable_general_properties_info.compliance_code.qsfp_sfp_cc.common_cc;

	printf("Compliance: ");
	telemetry_phy_print_SFP_cc(sfp_cc);
	telemetry_phy_print_QSFP_SFP_common_cc(common_cc);
	printf("\n");
}

/*
 * Print CMIS Only Compliance Code
 *
 * @module_info_struct [in]: Extracted Module info to print
 */
static void telemetry_phy_print_module_info_CMIS_only_compliance_code(
	struct doca_telemetry_phy_module_info *module_info_struct)
{
	enum doca_telemetry_phy_CMIS_common_cc common_cc =
		module_info_struct->cable_general_properties_info.compliance_code.cmis_cc.common_cc;

	printf("Compliance: ");
	telemetry_phy_print_CMIS_common_cc(common_cc);
	printf("\n");
}

/*
 * Print CMIS Active Compliance Code
 *
 * @module_info_struct [in]: Extracted CMIS Active Compliance Code to print
 */
static void telemetry_phy_print_module_info_CMIS_active_compliance_code(
	struct doca_telemetry_phy_module_info *module_info_struct)
{
	enum doca_telemetry_phy_CMIS_common_cc common_cc =
		module_info_struct->cable_general_properties_info.compliance_code.cmis_cc.common_cc;
	enum doca_telemetry_phy_CMIS_copper copper_cc =
		module_info_struct->cable_general_properties_info.compliance_code.cmis_cc.specific_cc.copper_cc;

	printf("Compliance: ");
	telemetry_phy_print_CMIS_common_cc(common_cc);
	telemetry_phy_print_CMIS_copper_cc(copper_cc);
	printf("\n");
}

/*
 * Print CMIS Optical Compliance Code
 *
 * @module_info_struct [in]: Extracted CMIS Optical Compliance Code to print
 */
static void telemetry_phy_print_module_info_CMIS_optical_compliance_code(
	struct doca_telemetry_phy_module_info *module_info_struct)
{
	enum doca_telemetry_phy_CMIS_common_cc common_cc =
		module_info_struct->cable_general_properties_info.compliance_code.cmis_cc.common_cc;
	enum doca_telemetry_phy_CMIS_optical_mm mm_cc =
		module_info_struct->cable_general_properties_info.compliance_code.cmis_cc.specific_cc.mm_cc;
	enum doca_telemetry_phy_CMIS_optical_sm sm_cc =
		module_info_struct->cable_general_properties_info.compliance_code.cmis_cc.specific_cc.sm_cc;

	printf("Compliance: ");
	telemetry_phy_print_CMIS_common_cc(common_cc);
	if (module_info_struct->cable_general_properties_info.cable_technology.QSFP_CMIS_cable_technology ==
	    DOCA_TELEMETRY_PHY_QSFP_CMIS_VCSEL_850NM) {
		telemetry_phy_print_CMIS_optical_mm_cc(mm_cc);
	} else if (module_info_struct->cable_general_properties_info.cable_technology.QSFP_CMIS_cable_technology >=
			   DOCA_TELEMETRY_PHY_QSFP_CMIS_VCSEL_1310NM &&
		   module_info_struct->cable_general_properties_info.cable_technology.QSFP_CMIS_cable_technology <=
			   DOCA_TELEMETRY_PHY_QSFP_CMIS_EML_1550NM) {
		telemetry_phy_print_CMIS_optical_sm_cc(sm_cc);
	}
	printf("\n");
}

/*
 * Print CMIS Cable Breakout info
 *
 * @cmis_cable_identifier [in]: Extracted CMIS Cable Identifier
 * @cable_breakout [in]: Extracted CMIS Cable Breakout to print
 */
static void telemetry_phy_print_module_info_CMIS_cable_breakout(
	enum doca_telemetry_phy_cable_identifier cmis_cable_identifier,
	enum doca_telemetry_phy_CMIS_cable_breakout cable_breakout)
{
	const char *identifier_string = NULL;

	printf("Cable Breakout: ");

	switch (cmis_cable_identifier) {
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_DD:
		identifier_string = "QSFP-DD";
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_OSFP:
		identifier_string = "OSFP";
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_DSFP:
		identifier_string = "DSFP";
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_CMIS:
		identifier_string = "QSFP_CMIS";
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_SFP_DD:
		identifier_string = "SFP-DD";
		break;
	default:
		printf("N/A\n");
		return;
	}

	switch (cable_breakout) {
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_TO_X:
		printf("%s%s%s", identifier_string, " to ", identifier_string);
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_TO_2QSFP_OR_2X_4LANES:
		printf("%s%s%s%s", identifier_string, " to 2xQSFP or 2x", identifier_string, " (depopulated / 4 lanes)");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_TO_4DSFP_OR_4QSFP_2LANES:
		printf("%s%s", identifier_string, " to 4xDSFP or 4xQSFP (depopulated / 2 lanes)");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_TO_8SFP:
		printf("%s%s", identifier_string, " to 8xSFP");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_4LANES_TO_QSFP_OR_X_4LANES:
		printf("%s%s%s%s",
		       identifier_string,
		       " (depopulated / 4 lanes) to QSFP or ",
		       identifier_string,
		       " (depopulated / 4 lanes)");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_4LANES_TO_2X_2LANES_OR_2SFPDD:
		printf("%s%s%s%s",
		       identifier_string,
		       " (depopulated / 4 lanes) to 2x",
		       identifier_string,
		       " (depopulated / 2 lanes) or 2xSFP-DD");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_4LANES_to_4SFP:
		printf("%s%s", identifier_string, " (depopulated / 4 lanes) to 4xSFP");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_2LANES_to_X:
		printf("%s%s%s", identifier_string, " (/ 2 lane module) to ", identifier_string);
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_2LANES_to_2SFP:
		printf("%s%s", identifier_string, " (/ 2 lane module) to 2xSFP");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_X_UNSPECIFIED:
		printf("Unspecified");
		break;
	case DOCA_TELEMETRY_PHY_CMIS_CABLE_BREAKOUT_NA:
	default:
		printf("N/A");
	}
	printf("\n");
}

/*
 * Print string of QSFP Cable Breakout Far End
 *
 * @far_end [in]: QSFP Cable Breakout Far End to print
 */
static void telemetry_phy_print_qsfp_cable_breakout_far_end(enum doca_telemetry_phy_QSFP_far_end far_end)
{
	printf("[Far End] ");
	switch (far_end) {
	case DOCA_TELEMETRY_PHY_QSFP_SINGLE_FAR_END_4_CHS_IMP_OR_SEPAR_MOD_4_CH_CONN:
		printf("Cable with single far-end with 4 channels implemented, or separable module with a 4-channel connector");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SINGLE_FAR_END_2_CHS_IMP_OR_SEPAR_MOD_2_CH_CONN:
		printf("Cable with single far-end with 2 channels implemented, or separable module with a 2-channel connector");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_SINGLE_FAR_END_1_CHS_IMP_OR_SEPAR_MOD_1_CH_CONN:
		printf("Cable with single far-end with 1 channels implemented, or separable module with a 1-channel connector");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_4_FAR_ENDS_1_CHS_IMP_IN_EACH:
		printf("4 far-ends with 1 channel implemented in each (i.e. 4x1 break out)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_2_FAR_ENDS_2_CHS_IMP_IN_EACH:
		printf("2 far-ends with 2 channels implemented in each (i.e. 2x2 break out)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_2_FAR_ENDS_1_CHS_IMP_IN_EACH:
		printf("2 far-ends with 1 channel implemented in each (i.e. 2x1 break out)");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_FAR_END_UNSPECIFIED:
		printf("Far end is unspecified");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_FAR_END_NA:
	default:
		printf("N/A");
	}
}

/*
 * Print string of QSFP Cable Breakout Near End
 *
 * @near_end [in]: QSFP Cable Breakout Near End to print
 */
static void telemetry_phy_print_qsfp_cable_breakout_near_end(enum doca_telemetry_phy_QSFP_near_end near_end)
{
	printf("[Near End] ");
	switch (near_end) {
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_IMP_CH2_IMP_CH3_IMP:
		printf("Channels implemented [1,2,3,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_IMP_CH2_IMP_CH3_IMP:
		printf("Channels implemented [2,3,4],Channels not implemented [1]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_NOT_IMP_CH2_IMP_CH3_IMP:
		printf("Channels implemented [1,3,4],Channels not implemented [2]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_NOTIMP_CH2_IMP_CH3_IMP:
		printf("Channels implemented [3,4],Channels not implemented [1,2]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_IMP_CH2_NOTIMP_CH3_IMP:
		printf("Channels implemented [1,2,4],Channels not implemented [3]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_IMP_CH2_NOTIMP_CH3_IMP:
		printf("Channels implemented [2,4],Channels not implemented [1,3]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_NOTIMP_CH2_NOTIMP_CH3_IMP:
		printf("Channels implemented [1,4],Channels not implemented [2,3]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_NOTIMP_CH2_NOTIMP_CH3_IMP:
		printf("Channels implemented [4],Channels not implemented [1,2,3]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_IMP_CH2_IMP_CH3_NOTIMP:
		printf("Channels implemented [1,2,3],Channels not implemented [4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_IMP_CH2_IMP_CH3_NOTIMP:
		printf("Channels implemented [2,3],Channels not implemented [1,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_NOTIMP_CH2_IMP_CH3_NOTIMP:
		printf("Channels implemented [1,3],Channels not implemented [2,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_NOTIMP_CH2_IMP_CH3_NOTIMP:
		printf("Channels implemented [3],Channels not implemented [1,2,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_IMP_CH2_NOTIMP_CH3_NOTIMP:
		printf("Channels implemented [1,2],Channels not implemented [3,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_IMP_CH2_NOTIMP_CH3_NOTIMP:
		printf("Channels implemented [2],Channels not implemented [1,3,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_IMP_CH1_NOTIMP_CH2_NOTIMP_CH3_NOTIMP:
		printf("Channels implemented [1],Channels not implemented [2,3,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_CH0_NOTIMP_CH1_NOTIMP_CH2_NOTIMP_CH3_NOTIMP:
		printf("Channels not implemented [1,2,3,4]");
		break;
	case DOCA_TELEMETRY_PHY_QSFP_NEAR_END_NA:
	default:
		printf("N/A");
	}
}

/*
 * Print string of QSFP Cable Breakout info
 *
 * @qsfp_cable_breakout [in]: Extracted QSFP Cable Breakout to print
 */
static void telemetry_phy_print_module_info_QSFP_cable_breakout(
	struct doca_telemetry_phy_QSFP_cable_breakout qsfp_cable_breakout)
{
	printf("Cable Breakout: ");
	telemetry_phy_print_qsfp_cable_breakout_near_end(qsfp_cable_breakout.near_end);
	telemetry_phy_print_qsfp_cable_breakout_far_end(qsfp_cable_breakout.far_end);
	printf("\n");
}

/*
 * Print monitor info
 *
 * Print the contents of the extracted Module info.
 *
 * @module_info_struct [in]: Extracted module info struct to print
 */
static void telemetry_phy_print_module_info(struct doca_telemetry_phy_module_info *module_info_struct)
{
	uint8_t is_CMIS = (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_SFP_DD) ||
			  (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_DD) ||
			  (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_CMIS) ||
			  (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_OSFP) ||
			  (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_DSFP);
	uint8_t is_QSFP = (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP28) ||
			  (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_PLUS) ||
			  (module_info_struct->cable_general_properties_info.cable_identifier ==
			   DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_SPLIT_CABLE);
	uint8_t is_SFP = (module_info_struct->cable_general_properties_info.cable_identifier ==
			  DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_SFP28_OR_SFP_PLUS) ||
			 (module_info_struct->cable_general_properties_info.cable_identifier ==
			  DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSA_QSFP_SFP);
	uint8_t is_eth = !!(module_info_struct->active_protocol == DOCA_TELEMETRY_PHY_PROTOCOL_ETH);
	uint8_t is_active = !!(module_info_struct->cable_general_properties_info.cable_type ==
			       DOCA_TELEMETRY_PHY_CABLE_TYPE_ACTIVE_CABLE);
	uint8_t is_optical_module = !!(module_info_struct->cable_general_properties_info.cable_type ==
				       DOCA_TELEMETRY_PHY_CABLE_TYPE_OPTICAL_MODULE);

	printf("Module info\n");
	printf("-----------\n");

	if (module_info_struct->cable_power_and_temp_info.ddm_supported &&
	    !((module_info_struct->cable_power_and_temp_info.module_temperature.temperature_low == 0) &&
	      (module_info_struct->cable_power_and_temp_info.module_temperature.temperature_high == 0))) {
		printf("Temperature [C]: %d, Alarm threshold low/high: %d/%d\n",
		       module_info_struct->cable_power_and_temp_info.module_temperature.temperature,
		       module_info_struct->cable_power_and_temp_info.module_temperature.temperature_low,
		       module_info_struct->cable_power_and_temp_info.module_temperature.temperature_high);
	} else {
		printf("Temperature [C]: N/A\n");
	}

	if (module_info_struct->cable_power_and_temp_info.ddm_supported &&
	    !((module_info_struct->cable_power_and_temp_info.module_voltage.voltage_low == 0) &&
	      (module_info_struct->cable_power_and_temp_info.module_voltage.voltage_high == 0))) {
		printf("Voltage [mV]: %d, Alarm threshold low/high: %d/%d\n",
		       module_info_struct->cable_power_and_temp_info.module_voltage.voltage,
		       module_info_struct->cable_power_and_temp_info.module_voltage.voltage_low,
		       module_info_struct->cable_power_and_temp_info.module_voltage.voltage_high);
	} else {
		printf("Voltage [mV]: N/A\n");
	}

	printf("Cable identifier: ");
	switch (module_info_struct->cable_general_properties_info.cable_identifier) {
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP28:
		printf("QSFP28\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_PLUS:
		printf("QSFP+\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_SFP28_OR_SFP_PLUS:
		printf("SFP28 OR SFP+\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSA_QSFP_SFP:
		printf("QSA_QSFP_SFP\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_BLACKPLANE:
		printf("BLACKPLANE\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_SFP_DD:
		printf("SFP_DD\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_DD:
		printf("QSFP_DD\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_CMIS:
		printf("QSFP_CMIS\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_OSFP:
		printf("OSFP\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_C2C:
		printf("C2C\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_DSFP:
		printf("DSFP\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_QSFP_SPLIT_CABLE:
		printf("QSFP SPLIT CABLE\n");
		break;
	case DOCA_TELEMETRY_PHY_CABLE_IDENTIFIER_NA:
	default:
		printf("N/A\n");
	}

	printf("Cable technology: ");
	if (is_CMIS || is_QSFP) {
		telemetry_phy_print_module_info_QSFP_CMIS_cable_technology(
			module_info_struct->cable_general_properties_info.cable_technology.QSFP_CMIS_cable_technology);
	} else if (is_SFP) {
		telemetry_phy_print_module_info_SFP_cable_technology(
			module_info_struct->cable_general_properties_info.cable_technology.SFP_cable_technology);
	} else {
		printf("N/A\n");
	}

	telemetry_phy_print_module_info_cable_type(module_info_struct->cable_general_properties_info.cable_type);
	telemetry_phy_print_module_info_cable_vendor(module_info_struct->cable_vendor_info.cable_vendor);
	printf("Vendor Name:\t%s\n", module_info_struct->cable_vendor_info.vendor_name);

	if (module_info_struct->cable_power_and_temp_info.ddm_supported) {
		printf("Digital Diagnostic Monitoring: Supported\n");
	} else {
		printf("Digital Diagnostic Monitoring: Not supported\n");
	}

	if (is_CMIS) {
		if (is_active) {
			telemetry_phy_print_module_info_CMIS_active_compliance_code(module_info_struct);
		} else if (is_optical_module) {
			telemetry_phy_print_module_info_CMIS_optical_compliance_code(module_info_struct);
		} else {
			telemetry_phy_print_module_info_CMIS_only_compliance_code(module_info_struct);
		}
	} else if (is_eth) {
		if (is_QSFP) {
			telemetry_phy_print_module_info_eth_QSFP_compliance_code(module_info_struct);
		} else if (is_SFP) {
			telemetry_phy_print_module_info_eth_SFP_compliance_code(module_info_struct);
		}
	} else {
		printf("Compliance: N/A\n");
	}

	if (is_CMIS) {
		telemetry_phy_print_module_info_CMIS_cable_breakout(
			module_info_struct->cable_general_properties_info.cable_identifier,
			module_info_struct->cable_general_properties_info.cable_breakout.cmis_cable_breakout);
	} else if (is_QSFP) {
		telemetry_phy_print_module_info_QSFP_cable_breakout(
			module_info_struct->cable_general_properties_info.cable_breakout.qsfp_cable_breakout);
	} else {
		printf("Cable Breakout: N/A\n");
	}

	printf("\n");
}

/*
 * Print counter and BER info
 *
 * Print the contents of the extracted counter and BER info.
 *
 * @counter_and_ber_info_struct [in]: Extracted counter and BER info struct to print
 */
static void telemetry_phy_print_counter_and_ber_info(
	struct doca_telemetry_phy_counter_and_ber_info *counter_and_ber_info_struct)
{
	printf("counter and BER info\n");
	printf("-----------\n");

	printf("Time Since Last Clear [Min]: %.1f\n",
	       (double)(counter_and_ber_info_struct->time_since_last_clear) / TIME_CONVERT_MS_MIN);

	printf("Link Down Counter: %u\n", counter_and_ber_info_struct->link_down_counter);
	printf("Link Error Recovery Counter: %u\n", counter_and_ber_info_struct->link_down_recovery_counter);

	if (counter_and_ber_info_struct->active_protocol == DOCA_TELEMETRY_PHY_PROTOCOL_IB) {
		printf("Symbol Errors: %lu\n", counter_and_ber_info_struct->symbol_errors.symbol_errors_counter);
		printf("Symbol BER: %uE-%u\n",
		       counter_and_ber_info_struct->symbol_errors.symbol_ber.ber_coef,
		       counter_and_ber_info_struct->symbol_errors.symbol_ber.ber_magnitude);
	}
	printf("Effective Physical Errors: %lu\n",
	       counter_and_ber_info_struct->effective_errors.effective_errors_counter);
	printf("Effective Physical BER: %uE-%u\n",
	       counter_and_ber_info_struct->effective_errors.effective_ber.ber_coef,
	       counter_and_ber_info_struct->effective_errors.effective_ber.ber_magnitude);

	printf("Raw Physical Errors Per Lane: ");
	for (uint8_t lane_module_idx = 0; lane_module_idx < counter_and_ber_info_struct->number_of_lanes;
	     lane_module_idx++) {
		if (lane_module_idx != 0) {
			printf(",");
		}
		printf("%lu", counter_and_ber_info_struct->raw_errors.raw_errors_per_lane[lane_module_idx]);
	}

	printf("\nRaw Physical BER Per Lane: ");
	for (uint8_t lane_module_idx = 0; lane_module_idx < counter_and_ber_info_struct->number_of_lanes;
	     lane_module_idx++) {
		if (lane_module_idx != 0) {
			printf(",");
		}
		printf("%uE-%u",
		       counter_and_ber_info_struct->raw_errors.raw_ber_per_lane[lane_module_idx].ber_coef,
		       counter_and_ber_info_struct->raw_errors.raw_ber_per_lane[lane_module_idx].ber_magnitude);
	}
	printf("\nRaw Physical BER: %uE-%u\n",
	       counter_and_ber_info_struct->raw_errors.raw_ber.ber_coef,
	       counter_and_ber_info_struct->raw_errors.raw_ber.ber_magnitude);

	printf("\n");
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

	if (sample_objects->counter_and_ber_info_struct) {
		DOCA_LOG_INFO("counter_and_ber_info_struct %p: counter_and_ber_info_struct was destroyed",
			      sample_objects->counter_and_ber_info_struct);
		free(sample_objects->counter_and_ber_info_struct);
		sample_objects->counter_and_ber_info_struct = NULL;
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
		sample_objects->operation_info_struct = (struct doca_telemetry_phy_operation_info *)malloc(
			sizeof(struct doca_telemetry_phy_operation_info));
		if (sample_objects->operation_info_struct == NULL) {
			DOCA_LOG_ERR("Failed to allocate output objects: failed to allocate memory for operation info");
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	if (cfg->get_module_info) {
		sample_objects->module_info_struct =
			(struct doca_telemetry_phy_module_info *)malloc(sizeof(struct doca_telemetry_phy_module_info));
		if (sample_objects->module_info_struct == NULL) {
			DOCA_LOG_ERR("Failed to allocate output objects: failed to allocate memory for module info");
			return DOCA_ERROR_NO_MEMORY;
		}
	}

	if (cfg->get_counter_and_ber_info) {
		sample_objects->counter_and_ber_info_struct = (struct doca_telemetry_phy_counter_and_ber_info *)malloc(
			sizeof(struct doca_telemetry_phy_counter_and_ber_info));
		if (sample_objects->counter_and_ber_info_struct == NULL) {
			DOCA_LOG_ERR(
				"Failed to allocate output objects: failed to allocate memory for counter and BER info");
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

	if (cfg->get_counter_and_ber_info) {
		result = doca_telemetry_phy_cap_counter_and_ber_info_is_supported(devinfo);
		if (result == DOCA_ERROR_NOT_SUPPORTED) {
			DOCA_LOG_ERR(
				"Failed to start telemetry_phy: device does not support doca_telemetry_phy counter and BER info");
			return DOCA_ERROR_NOT_SUPPORTED;
		} else if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to start telemetry_phy: failed to query capability for counter and BER info");
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
	if (cfg->get_counter_and_ber_info) {
		DOCA_LOG_INFO("	Retrieve counter and BER info");
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

		telemetry_phy_print_operation_info(sample_objects.operation_info_struct);
	}

	if (cfg->get_module_info) {
		result = doca_telemetry_phy_get_module_info(sample_objects.telemetry_phy_obj,
							    sample_objects.module_info_struct);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to read module info with error=%s", doca_error_get_name(result));
			goto teardown;
		}

		telemetry_phy_print_module_info(sample_objects.module_info_struct);
	}

	if (cfg->get_counter_and_ber_info) {
		result = doca_telemetry_phy_get_counter_and_ber_info(sample_objects.telemetry_phy_obj,
								     sample_objects.counter_and_ber_info_struct);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to read counter and BER info with error=%s", doca_error_get_name(result));
			goto teardown;
		}

		telemetry_phy_print_counter_and_ber_info(sample_objects.counter_and_ber_info_struct);
	}

teardown:
	teardown_result = telemetry_phy_sample_cleanup(&sample_objects);
	if (teardown_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Teardown failed with error=%s", doca_error_get_name(teardown_result));
		DOCA_ERROR_PROPAGATE(result, teardown_result);
	}

	return result;
}
