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

#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include <devemu_pci_type_config.h>
#include <doca_devemu_pci_info.h>
#include <doca_devemu_vnet_type.h>
#include <doca_devemu_vblk_type.h>
#include "devemu_pci_device_tlp_bridge_handler_config.h"

DOCA_LOG_REGISTER(DEVEMU_PCI_TLP_BRIDGE_HANDLER::SAMPLE);

/* Shared variable to allow for a proper shutdown */
static volatile bool force_quit;

struct pci_type_conf {
	const char *name;
	doca_error_t (*type_create_func)(const char *name, struct doca_devemu_pci_type **pci_type);
	bool is_custom_bar_layout;
	/*
	 * Shift all bar regions (defined in devemu_pci_type_config.h) 4KB right,
	 * to get a different bar layout and maximize resource utilization.
	 */
	bool shift_all_bar_regions_4KB_right;
};

static const struct pci_type_conf pci_type_confs[MAX_TLP_PCI_TYPE_NUM] = {
	{
		.name = "PCI Type 1 custom bar layout 1",
		.type_create_func = doca_devemu_pci_tlp_type_create,
		.is_custom_bar_layout = true,
		.shift_all_bar_regions_4KB_right = false,
	},
	{
		.name = "PCI Type 2 custom bar layout 2",
		.type_create_func = doca_devemu_pci_tlp_type_create,
		.is_custom_bar_layout = true,
		.shift_all_bar_regions_4KB_right = true,
	},
	{
		.name = "PCI Type 3 vnet bar layout",
		.type_create_func = doca_devemu_vnet_pci_tlp_type_create,
		.is_custom_bar_layout = false,
	},
	{
		.name = "PCI Type 4 vblk bar layout",
		.type_create_func = doca_devemu_vblk_pci_tlp_type_create,
		.is_custom_bar_layout = false,
	},
	{
		.name = "PCI Type 5 custom bar layout 1",
		.type_create_func = doca_devemu_pci_tlp_type_create,
		.is_custom_bar_layout = true,
		.shift_all_bar_regions_4KB_right = false,
	},
	{
		.name = "PCI Type 6 custom bar layout 2",
		.type_create_func = doca_devemu_pci_tlp_type_create,
		.is_custom_bar_layout = true,
		.shift_all_bar_regions_4KB_right = true,
	},
	{
		.name = "PCI Type 7 vnet bar layout",
		.type_create_func = doca_devemu_vnet_pci_tlp_type_create,
		.is_custom_bar_layout = false,
	},
	{
		.name = "PCI Type 8 vblk bar layout",
		.type_create_func = doca_devemu_vblk_pci_tlp_type_create,
		.is_custom_bar_layout = false,
	},
};

/*
 * Check if stdin has input available without blocking
 *
 * @return: true if input is available, false otherwise
 */
static bool stdin_has_input(void)
{
	struct timeval tv = {0, 0};
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/*
 * Read and parse hotplug command from stdin (non-blocking)
 *
 * @max_dsp [in]: Maximum DSP count
 * @dsp_idx [out]: Pointer to store the DSP index
 * @is_plug [out]: Pointer to store plug/unplug flag (true for plug, false for unplug)
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t read_hotplug_command(uint32_t max_dsp, uint32_t *dsp_idx, bool *is_plug)
{
	char line[128];
	char command[32];
	uint32_t idx;

	/* Read one line from stdin (non-blocking since stdin_has_input() was checked) */
	if (fgets(line, sizeof(line), stdin) == NULL) {
		/* EOF or error, clear stdin */
		clearerr(stdin);
		return DOCA_ERROR_IO_FAILED;
	}

	/* Parse the line */
	if (sscanf(line, "%31s %u", command, &idx) != 2) {
		DOCA_LOG_ERR("Invalid input format. Usage: plug <dsp_idx> or unplug <dsp_idx>");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (strcmp(command, "plug") == 0) {
		*is_plug = true;
	} else if (strcmp(command, "unplug") == 0) {
		*is_plug = false;
	} else {
		DOCA_LOG_ERR("Invalid command '%s'. Use 'plug' or 'unplug'", command);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (idx >= max_dsp) {
		DOCA_LOG_ERR("DSP index %u out of range (max: %u)", idx, max_dsp - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	*dsp_idx = idx;
	return DOCA_SUCCESS;
}

/*
 * Enqueue ACG credit to the circular queue
 *
 * @tlp_ctx [in]: TLP context
 * @acg_req [in]: ACG request to enqueue
 * @return: DOCA_SUCCESS on success, DOCA_ERROR_NO_MEMORY if queue is full or NULL
 */
static doca_error_t acg_queue_push(struct tlp_context *tlp_ctx, struct doca_devemu_pci_tlp_channel_req *acg_req)
{
	if (tlp_ctx->acg_queue == NULL)
		return DOCA_ERROR_NO_MEMORY;

	if (tlp_ctx->acg_queue_count >= tlp_ctx->acg_queue_size)
		return DOCA_ERROR_NO_MEMORY;

	tlp_ctx->acg_queue[tlp_ctx->acg_queue_tail] = acg_req;
	tlp_ctx->acg_queue_tail = (tlp_ctx->acg_queue_tail + 1) % tlp_ctx->acg_queue_size;
	tlp_ctx->acg_queue_count++;
	return DOCA_SUCCESS;
}

/*
 * Dequeue ACG credit from the circular queue
 *
 * @tlp_ctx [in]: TLP context
 * @return: ACG request pointer, or NULL if queue is empty or NULL
 */
static struct doca_devemu_pci_tlp_channel_req *acg_queue_pop(struct tlp_context *tlp_ctx)
{
	if (tlp_ctx->acg_queue == NULL || tlp_ctx->acg_queue_count == 0)
		return NULL;

	struct doca_devemu_pci_tlp_channel_req *acg_req = tlp_ctx->acg_queue[tlp_ctx->acg_queue_head];
	tlp_ctx->acg_queue_head = (tlp_ctx->acg_queue_head + 1) % tlp_ctx->acg_queue_size;
	tlp_ctx->acg_queue_count--;
	return acg_req;
}

/*
 * Signal handler for graceful shutdown
 *
 * @signum [in]: Signal number to handle
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		force_quit = true;
	}
}

/*
 * Initialize PCI capabilities for endpoint (matches tlp_handler implementation)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_endpoint_capabilities(struct pci_device_config *dev_cfg)
{
	/* Express capability */
	dev_cfg->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dev_cfg->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_VPD;
	dev_cfg->caps.express.pcie_cap_register = 0x0002;
	dev_cfg->caps.express.dev_capabilities = 0x112c8fe2;
	dev_cfg->caps.express.dev_control = 0x2950;
	dev_cfg->caps.express.dev_status = 0x0000;
	dev_cfg->caps.express.link_capabilities = 0x00500104;
	dev_cfg->caps.express.link_control = 0x0000;
	dev_cfg->caps.express.link_status = 0x3104;
	dev_cfg->caps.express.slot_capabilities = 0;
	dev_cfg->caps.express.slot_control = 0;
	dev_cfg->caps.express.slot_status = 0;
	dev_cfg->caps.express.root_control = 0;
	dev_cfg->caps.express.root_capabilities = 0;
	dev_cfg->caps.express.root_status = 0;
	dev_cfg->caps.express.dev_capabilities2 = 0x00030397;
	dev_cfg->caps.express.dev_control2 = 0;
	dev_cfg->caps.express.dev_status2 = 0;
	dev_cfg->caps.express.link_capabilities2 = 0x0180003e;
	dev_cfg->caps.express.link_control2 = 0;
	dev_cfg->caps.express.link_status2 = 0;
	dev_cfg->caps.express.slot_capabilities2 = 0;
	dev_cfg->caps.express.slot_control2 = 0;
	dev_cfg->caps.express.slot_status2 = 0;

	/* VPD capability */
	dev_cfg->caps.vpd.cap_id = TLP_PCI_CAP_ID_VPD;
	dev_cfg->caps.vpd.next_cap_ptr = TLP_PCI_CAP_OFFSET_MSIX;
	dev_cfg->caps.vpd.addr_register = 0x8000;     /* F bit (bit 15) = 1, data ready */
	dev_cfg->caps.vpd.data_register = 0x00000078; /* VPD End tag */

	/* MSI-X capability */
	dev_cfg->caps.msix.cap_id = TLP_PCI_CAP_ID_MSIX;
	dev_cfg->caps.msix.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dev_cfg->caps.msix.message_control = 0x0000;

	/* PM capability */
	dev_cfg->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dev_cfg->caps.pm.next_cap_ptr = 0x00;
	dev_cfg->caps.pm.pmc = 0xC803;
	dev_cfg->caps.pm.pmcsr = 0x0008;
	dev_cfg->caps.pm.reserved = 0x00;
	dev_cfg->caps.pm.data = 0x00;
}

/*
 * Initialize USP (Upstream Port) capabilities
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_usp_capabilities(struct pci_device_config *dev_cfg)
{
	/* Express capability for upstream port */
	dev_cfg->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dev_cfg->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dev_cfg->caps.express.pcie_cap_register = 0x0052;    /* Type 5: Upstream Port, No Slot, Version=2 */
	dev_cfg->caps.express.dev_capabilities = 0x00008001; /* Max_Payload_Size=128, Extended Tag */
	dev_cfg->caps.express.dev_control = 0x0000;
	dev_cfg->caps.express.dev_status = 0x0000;
	dev_cfg->caps.express.link_capabilities = 0x00500104;
	dev_cfg->caps.express.link_control = 0x0000;
	dev_cfg->caps.express.link_status = 0x1104;
	dev_cfg->caps.express.slot_capabilities = 0;
	dev_cfg->caps.express.slot_control = 0;
	dev_cfg->caps.express.slot_status = 0;

	dev_cfg->caps.express.root_control = 0;
	dev_cfg->caps.express.root_capabilities = 0;
	dev_cfg->caps.express.root_status = 0;
	dev_cfg->caps.express.dev_capabilities2 = 0;
	dev_cfg->caps.express.dev_control2 = 0;
	dev_cfg->caps.express.dev_status2 = 0;
	dev_cfg->caps.express.link_capabilities2 = 0;
	dev_cfg->caps.express.link_control2 = 0;
	dev_cfg->caps.express.link_status2 = 0;
	dev_cfg->caps.express.slot_capabilities2 = 0;
	dev_cfg->caps.express.slot_control2 = 0;
	dev_cfg->caps.express.slot_status2 = 0;

	/* PM capability */
	dev_cfg->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dev_cfg->caps.pm.next_cap_ptr = 0x00;
	dev_cfg->caps.pm.pmc = 0xC803;
	dev_cfg->caps.pm.pmcsr = 0x0008;
	dev_cfg->caps.pm.reserved = 0x00;
	dev_cfg->caps.pm.data = 0x00;

	/* VPD and MSI not needed for USP */
	memset(&dev_cfg->caps.vpd, 0, sizeof(dev_cfg->caps.vpd));
	memset(&dev_cfg->caps.msi, 0, sizeof(dev_cfg->caps.msi));
}

/*
 * Initialize PCI capabilities for DSP bridge (for hotplug support)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_dsp_capabilities(struct pci_device_config *dev_cfg)
{
	/* Express capability for bridge with hotplug support */
	dev_cfg->caps.express.cap_id = TLP_PCI_CAP_ID_EXPRESS;
	dev_cfg->caps.express.next_cap_ptr = TLP_PCI_CAP_OFFSET_MSI;
	dev_cfg->caps.express.pcie_cap_register = 0x0162;    /* Type 6: DSP, Slot Implemented=1, Version=2 */
	dev_cfg->caps.express.dev_capabilities = 0x00008001; /* Max_Payload_Size=128, Extended Tag */
	dev_cfg->caps.express.dev_control = 0x0000;
	dev_cfg->caps.express.dev_status = 0x0000;
	dev_cfg->caps.express.link_capabilities = 0x01500104;
	dev_cfg->caps.express.link_control = 0x0000;
	dev_cfg->caps.express.link_status = 0x1104;
	/* Slot Capabilities: AttnBtn=0, PwrCtrl=1, HotPlug=1, Surprise=1, NoCompl=0 */
	dev_cfg->caps.express.slot_capabilities = TLP_BRIDGE_SLOT_CAPABILITIES;
	/*
	 * Slot Control: Let Host set enable bits via pcie_enable_notification()
	 * Only initialize Power OFF (bit10=1)
	 */
	dev_cfg->caps.express.slot_control = SLOT_CTRL_POWER_CONTROLLER;
	dev_cfg->caps.express.slot_status = 0x0000;

	dev_cfg->caps.express.root_control = 0;
	dev_cfg->caps.express.root_capabilities = 0;
	dev_cfg->caps.express.root_status = 0;
	dev_cfg->caps.express.dev_capabilities2 = 0;
	dev_cfg->caps.express.dev_control2 = 0;
	dev_cfg->caps.express.dev_status2 = 0;
	dev_cfg->caps.express.link_capabilities2 = 0;
	dev_cfg->caps.express.link_control2 = 0;
	dev_cfg->caps.express.link_status2 = 0;
	dev_cfg->caps.express.slot_capabilities2 = 0;
	dev_cfg->caps.express.slot_control2 = 0;
	dev_cfg->caps.express.slot_status2 = 0;

	dev_cfg->caps.msi.cap_id = TLP_PCI_CAP_ID_MSI;
	dev_cfg->caps.msi.next_cap_ptr = TLP_PCI_CAP_OFFSET_PM;
	dev_cfg->caps.msi.message_control = 0x0080;
	dev_cfg->caps.msi.message_address_low = 0x00000000;
	dev_cfg->caps.msi.message_address_high = 0x00000000;
	dev_cfg->caps.msi.message_data = 0x0000;
	dev_cfg->caps.msi.reserved = 0x0000;
	dev_cfg->caps.msi.mask_bits = 0x00000000;
	dev_cfg->caps.msi.pending_bits = 0x00000000;

	/* PM capability */
	dev_cfg->caps.pm.cap_id = TLP_PCI_CAP_ID_PM;
	dev_cfg->caps.pm.next_cap_ptr = 0x00;
	dev_cfg->caps.pm.pmc = 0xC803;
	dev_cfg->caps.pm.pmcsr = 0x0008;
	dev_cfg->caps.pm.reserved = 0x00;
	dev_cfg->caps.pm.data = 0x00;

	memset(&dev_cfg->caps.vpd, 0, sizeof(dev_cfg->caps.vpd));
}

/*
 * Initialize default Type 0 configuration space header (Endpoint)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_type0_header(struct pci_device_config *dev_cfg)
{
	dev_cfg->cfg_space_hdr.type0.vendor_id = TLP_PCI_TYPE_VENDOR_ID;
	dev_cfg->cfg_space_hdr.type0.device_id = TLP_PCI_TYPE_ENDPOINT_DEVICE_ID;
	dev_cfg->cfg_space_hdr.type0.command = 0x0000;
	dev_cfg->cfg_space_hdr.type0.status = 0x0010; /* Capabilities list present */
	dev_cfg->cfg_space_hdr.type0.class_code = TLP_PCI_CLASS_CODE_ENDPOINT;
	dev_cfg->cfg_space_hdr.type0.revision_id = TLP_PCI_TYPE_REVISION_ID;
	dev_cfg->cfg_space_hdr.type0.bist = 0x00;
	dev_cfg->cfg_space_hdr.type0.header_type = HEADER_TYPE_ENDPOINT;
	dev_cfg->cfg_space_hdr.type0.latency_timer = 0x00;
	dev_cfg->cfg_space_hdr.type0.cache_line_size = 0x10;

	/* BAR configuration - endpoints will have BAR0-1 as 64-bit prefetchable memory */
	dev_cfg->cfg_space_hdr.type0.bar[0] = 0x00000000 | BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT |
					      BAR_MEM_PREFETCHABLE;
	dev_cfg->cfg_space_hdr.type0.bar[1] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[2] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[3] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[4] = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.bar[5] = 0x00000000;

	dev_cfg->cfg_space_hdr.type0.cardbus_cis_pointer = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.subsystem_vendor_id = TLP_PCI_TYPE_SUBSYSTEM_VENDOR_ID;
	dev_cfg->cfg_space_hdr.type0.subsystem_id = TLP_PCI_TYPE_SUBSYSTEM_ID;
	dev_cfg->cfg_space_hdr.type0.exp_rom_base_addr = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.cap_ptr = 0x60; /* First capability at offset 0x60 (Express Cap) */
	dev_cfg->cfg_space_hdr.type0.reserved_at_14 = 0x00000000;
	dev_cfg->cfg_space_hdr.type0.interrupt_line = 0x00;
	dev_cfg->cfg_space_hdr.type0.interrupt_pin = 0x00;
	dev_cfg->cfg_space_hdr.type0.min_grant = 0x00;
	dev_cfg->cfg_space_hdr.type0.max_latency = 0x00;
}

/*
 * Initialize default Type 1 configuration space header (Bridge)
 *
 * @dev_cfg [in/out]: Device configuration to initialize
 */
static void init_type1_header(struct pci_device_config *dev_cfg)
{
	dev_cfg->cfg_space_hdr.type1.vendor_id = TLP_PCI_TYPE_VENDOR_ID;
	dev_cfg->cfg_space_hdr.type1.device_id = TLP_PCI_TYPE_BRIDGE_DEVICE_ID;
	dev_cfg->cfg_space_hdr.type1.command = 0x0400; /* Bus Master Enable */
	dev_cfg->cfg_space_hdr.type1.status = 0x0010;  /* Capabilities list present */
	dev_cfg->cfg_space_hdr.type1.class_code = TLP_PCI_CLASS_CODE_BRIDGE;
	dev_cfg->cfg_space_hdr.type1.revision_id = TLP_PCI_TYPE_REVISION_ID;
	dev_cfg->cfg_space_hdr.type1.bist = 0x00;
	dev_cfg->cfg_space_hdr.type1.header_type = HEADER_TYPE_BRIDGE;
	dev_cfg->cfg_space_hdr.type1.latency_timer = 0x00;
	dev_cfg->cfg_space_hdr.type1.cache_line_size = 0x00;

	dev_cfg->cfg_space_hdr.type1.bar[0] = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.bar[1] = 0x00000000;
	/* Bus numbers - will be configured by Host during PCI enumeration */
	dev_cfg->cfg_space_hdr.type1.primary_bus = 0x00;
	dev_cfg->cfg_space_hdr.type1.secondary_bus = 0x00;
	dev_cfg->cfg_space_hdr.type1.subordinate_bus = 0x00;
	dev_cfg->cfg_space_hdr.type1.secondary_latency = 0x00;

	dev_cfg->cfg_space_hdr.type1.io_base = 0x00;
	dev_cfg->cfg_space_hdr.type1.io_limit = 0x00;
	dev_cfg->cfg_space_hdr.type1.secondary_status = 0x0000;
	dev_cfg->cfg_space_hdr.type1.memory_base = 0x0000;
	dev_cfg->cfg_space_hdr.type1.memory_limit = 0x0000;
	dev_cfg->cfg_space_hdr.type1.pre_memory_base = 0x0001;
	dev_cfg->cfg_space_hdr.type1.pre_memory_limit = 0x0001;
	dev_cfg->cfg_space_hdr.type1.pre_memory_base_upper_32bit = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.pre_memory_limit_upper_32bit = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.io_base_upper_16bit = 0x0000;
	dev_cfg->cfg_space_hdr.type1.io_limit_upper_16bit = 0x0000;
	dev_cfg->cfg_space_hdr.type1.cap_ptr = 0x60; /* First capability at offset 0x60 */
	dev_cfg->cfg_space_hdr.type1.exp_rom_base_addr = 0x00000000;
	dev_cfg->cfg_space_hdr.type1.interrupt_line = 0x00;
	dev_cfg->cfg_space_hdr.type1.interrupt_pin = 0x00;
	dev_cfg->cfg_space_hdr.type1.bridge_control = 0x0000;
}

/*
 * Get the DSP/EP group bounds for a given NV switch TLP DSP
 *
 * @tlp_ctx [in]: TLP context
 * @dsp_id [in]: NV switch TLP DSP index
 * @start [out]: Index of first DSP for this NV switch TLP DSP in devs_config
 * @count [out]: Number of DSPs (and EPs) assigned to this NV switch TLP DSP
 */
static inline void get_dsp_group(const struct tlp_context *tlp_ctx, uint8_t dsp_id, uint32_t *start, uint32_t *count)
{
	uint32_t base = tlp_ctx->num_ep_per_nv_switch_tlp_dsp;
	uint32_t extra = tlp_ctx->num_ep_extra_per_nv_switch_tlp_dsp;

	*count = base + (dsp_id < extra ? 1 : 0);
	*start = FIRST_DSP_IDX(tlp_ctx) + dsp_id * base + (dsp_id < extra ? dsp_id : extra);
}

/*
 * Initialize PCI device topology
 *
 * @tlp_ctx [in/out]: TLP context containing device array
 */
static void init_device_topology(struct tlp_context *tlp_ctx)
{
	memset(tlp_ctx->devs_config, 0, tlp_ctx->num_devices * sizeof(struct pci_device_config));

	/*
	 * Initialize one PCIe switch per NV switch TLP DSP: USP + its DSP bridges + their EPs.
	 * Bus numbers are left at 0 and will be configured by the Host during PCI enumeration.
	 */
	for (uint32_t i = 0; i < tlp_ctx->num_nv_switch_tlp_dsp; i++) {
		struct pci_device_config *usp = &tlp_ctx->devs_config[USP_IDX(tlp_ctx, i)];

		init_type1_header(usp);
		init_usp_capabilities(usp);
		usp->is_bridge = true;

		uint32_t group_start = 0;
		uint32_t group_count = 0;
		get_dsp_group(tlp_ctx, i, &group_start, &group_count);
		uint32_t ep_base = group_start - FIRST_DSP_IDX(tlp_ctx);

		for (uint32_t j = 0; j < group_count; j++) {
			struct pci_device_config *dsp = &tlp_ctx->devs_config[group_start + j];
			struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + ep_base + j];

			init_type1_header(dsp);
			init_dsp_capabilities(dsp);
			dsp->is_bridge = true;
			dsp->device = j; /* local index within this NV switch TLP DSP's group */

			init_type0_header(ep);
			init_endpoint_capabilities(ep);
			ep->is_endpoint = true;
		}
	}

	/* Initialize dummy device */
	struct pci_device_config *dummy = &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
	init_type0_header(dummy);
	dummy->is_dummy = true;

	DOCA_LOG_INFO("Initialized topology: %u bridges (%u USP + %u DSP), %u endpoints",
		      tlp_ctx->num_bridges,
		      tlp_ctx->num_nv_switch_tlp_dsp,
		      tlp_ctx->num_dsp,
		      tlp_ctx->num_ep);
	DOCA_LOG_INFO("  - %u Single-PF devices (function 0 each)", tlp_ctx->num_ep);
	DOCA_LOG_INFO("Bus numbers will be configured by Host during PCI enumeration");
	DOCA_LOG_INFO("Host will write bus numbers via Type 1 config write to bridges");
}

/*
 * Update DSP Slot Status when device is plugged/unplugged
 *
 * @dsp [in/out]: DSP bridge device
 * @device_present [in]: True if device present, false if removed
 */
static void update_dsp_slot_status(struct pci_device_config *dsp, bool device_present)
{
	/*
	 * ABP (Attention Button Pressed) mode:
	 * - Plug: Set ABP + PDC + PDS, wait for Host to Power ON
	 * - Unplug: Set ABP only, let Host handle the removal sequence
	 */
	dsp->caps.express.slot_status |= SLOT_STS_ATTN_BTN_PRESSED;

	if (device_present) {
		/* Plug: ABP + PDC + PDS */
		dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_CHANGED;
		dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_STATE;
		/* Do NOT set DLActive - wait for Host to write Power ON */
		DOCA_LOG_INFO("Device plugged (ABP mode): ABP=1, PDC=1, PDS=1, DLActive=0");
	} else {
		/*
		 * Unplug: ABP only
		 * - Keep PDS=1 (let Host detect via ABP, not presence change)
		 * - Clear DLActive to indicate link down
		 * - Host will wait 5 seconds then unbind driver
		 */
		dsp->caps.express.link_status &= ~LINK_STS_DL_ACTIVE;
		DOCA_LOG_INFO("Device unplugged (ABP mode): ABP=1, PDS=1, DLActive=0 (Host will handle)");
	}

	DOCA_LOG_DBG("DSP Slot Status updated: present=%d, status=0x%04X, link_status=0x%04X",
		     device_present,
		     dsp->caps.express.slot_status,
		     dsp->caps.express.link_status);
}

/*
 * Get the next PCI device type from the TLP context in round-robin order.
 * Thread-safe: uses atomic increment to distribute types across concurrent callers.
 *
 * @tlp_ctx [in]: TLP context containing the device type array
 * @return: Pointer to the next doca_devemu_pci_type in rotation
 */
static struct doca_devemu_pci_type *get_dev_type(struct tlp_context *tlp_ctx)
{
	if (tlp_ctx == NULL || tlp_ctx->num_dev_types == 0) {
		DOCA_LOG_ERR("Invalid TLP context or no device types. tlp_ctx=%p", tlp_ctx);
		return NULL;
	}
	uint32_t idx = __atomic_fetch_add(&tlp_ctx->dev_type_idx, 1, __ATOMIC_SEQ_CST);
	return tlp_ctx->pci_type[idx % tlp_ctx->num_dev_types];
}

/*
 * Populate device configuration with BAR layout information from the PCI type.
 * Queries the type for BAR size, transaction region, and MSI-X table/PBA region
 * start addresses, and stores them in dev_cfg for use in config space and TLP handling.
 *
 * @dev_type [in]: DOCA devemu PCI type to query for layout info
 * @dev_cfg [out]: Device configuration to fill with log_bar_size, transaction_region_start,
 *                 msix_region_start, msix_pba_region_start, and caps.msix offsets
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t populate_bar_layout_info_into_device(struct doca_devemu_pci_type *dev_type,
							 struct pci_device_config *dev_cfg)
{
	doca_error_t result;
	uint32_t n;
	uint8_t log_sz;
	uint64_t start_addr;
	struct doca_devemu_pci_bar_info **bar_list;
	struct doca_devemu_pci_transaction_region_info **mmio_list;
	struct doca_devemu_pci_msix_table_region_info **msix_list;
	struct doca_devemu_pci_msix_pba_region_info **pba_list;

	// Get bar size info
	result = doca_devemu_pci_type_create_bar_info_list(dev_type, &bar_list, &n);
	if (result != DOCA_SUCCESS || n != 2) {
		DOCA_LOG_ERR("Failed to create bar info list: %s, n: %d", doca_error_get_descr(result), n);
		return result;
	}
	doca_devemu_pci_bar_info_get_log_sz(bar_list[0], &log_sz);
	doca_devemu_pci_type_destroy_bar_info_list(bar_list);
	dev_cfg->log_bar_size = log_sz;

	// Get start address of transaction region
	result = doca_devemu_pci_type_create_transaction_region_info_list(dev_type, &mmio_list, &n);
	if (result != DOCA_SUCCESS || n != 1) {
		DOCA_LOG_ERR("Failed to create transaction region info list: %s, n: %d",
			     doca_error_get_descr(result),
			     n);
		return result;
	}
	doca_devemu_pci_transaction_region_info_get_start_addr(mmio_list[0], &start_addr);
	doca_devemu_pci_type_destroy_transaction_region_info_list(mmio_list);
	dev_cfg->transaction_region_start = start_addr;

	// Get start address of MSI-X table region
	result = doca_devemu_pci_type_create_msix_table_region_info_list(dev_type, &msix_list, &n);
	if (result != DOCA_SUCCESS || n != 1) {
		DOCA_LOG_ERR("Failed to create msix table region info list: %s, n: %d",
			     doca_error_get_descr(result),
			     n);
		return result;
	}
	doca_devemu_pci_msix_table_region_info_get_start_addr(msix_list[0], &start_addr);
	doca_devemu_pci_type_destroy_msix_table_region_info_list(msix_list);
	dev_cfg->msix_region_start = start_addr;

	// Get start address of MSI-X PBA region
	result = doca_devemu_pci_type_create_msix_pba_region_info_list(dev_type, &pba_list, &n);
	if (result != DOCA_SUCCESS || n != 1) {
		DOCA_LOG_ERR("Failed to create msix pba region info list: %s, n: %d", doca_error_get_descr(result), n);
		return result;
	}
	doca_devemu_pci_msix_pba_region_info_get_start_addr(pba_list[0], &start_addr);
	doca_devemu_pci_type_destroy_msix_pba_region_info_list(pba_list);
	dev_cfg->msix_pba_region_start = start_addr;

	// Set MSI-X table and PBA offsets into the device config space
	dev_cfg->caps.msix.table_offset = dev_cfg->msix_region_start;
	dev_cfg->caps.msix.pba_offset = dev_cfg->msix_pba_region_start;
	return DOCA_SUCCESS;
}

/*
 * Create endpoint device (representor + TLP device)
 * Used by both static mode (batch creation) and hotplug mode (on-demand)
 *
 * @tlp_ctx [in]: TLP context
 * @endpoint [in/out]: Endpoint device configuration
 * @return: DOCA_SUCCESS on success, error otherwise
 */
static doca_error_t create_device(struct tlp_context *tlp_ctx, struct pci_device_config *endpoint)
{
	doca_error_t result;
	const struct doca_devinfo_rep *devinfo_rep;
	uint16_t vhca_id_16;
	char name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN];

	struct doca_devemu_pci_type *dev_type = get_dev_type(tlp_ctx);
	if (dev_type == NULL) {
		DOCA_LOG_ERR("Failed to get device type");
		return DOCA_ERROR_NOT_FOUND;
	}
	result = doca_devemu_pci_type_get_name(dev_type, &name);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get device type name: %s", doca_error_get_descr(result));
		return result;
	}

	result = populate_bar_layout_info_into_device(dev_type, endpoint);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to populate bar layout info into device: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Creating device basing on type: %s", name);
	/* Create device representor */
	result = doca_devemu_pci_type_create_rep(dev_type, &endpoint->rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create representor: %s", doca_error_get_descr(result));
		return result;
	}

	/* Get VHCA ID */
	devinfo_rep = doca_dev_rep_as_devinfo(endpoint->rep);
	result = doca_devinfo_rep_get_vhca_id(devinfo_rep, &vhca_id_16);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get VHCA ID: %s", doca_error_get_descr(result));
		doca_devemu_pci_type_destroy_rep(endpoint->rep);
		endpoint->rep = NULL;
		return result;
	}
	endpoint->vhca_id = vhca_id_16;

	/* Create TLP device */
	result = doca_devemu_pci_tlp_dev_create(dev_type, endpoint->rep, &endpoint->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create TLP device: %s", doca_error_get_descr(result));
		doca_devemu_pci_type_destroy_rep(endpoint->rep);
		endpoint->rep = NULL;
		return result;
	}

	/* Start TLP device */
	result = doca_devemu_pci_tlp_dev_start(endpoint->tlp_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start TLP device: %s", doca_error_get_descr(result));
		doca_devemu_pci_tlp_dev_destroy(endpoint->tlp_dev);
		doca_devemu_pci_type_destroy_rep(endpoint->rep);
		endpoint->rep = NULL;
		endpoint->tlp_dev = NULL;
		return result;
	}

	endpoint->device_present = true;
	DOCA_LOG_INFO("Device created: VHCA ID=0x%x", endpoint->vhca_id);

	return DOCA_SUCCESS;
}

/*
 * Destroy endpoint device (for both hotplug and static modes)
 *
 * @endpoint [in/out]: Endpoint device configuration
 * @return: DOCA_SUCCESS on success, error otherwise
 */
static doca_error_t destroy_device(struct pci_device_config *endpoint)
{
	doca_error_t result = DOCA_SUCCESS;

	DOCA_LOG_DBG("Destroying device: vhca_id=0x%x", endpoint->vhca_id);

	if (endpoint->tlp_dev != NULL) {
		result = doca_devemu_pci_tlp_dev_stop(endpoint->tlp_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop TLP device: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_DBG("TLP device stopped");

		result = doca_devemu_pci_tlp_dev_destroy(endpoint->tlp_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy TLP device: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_DBG("TLP device destroyed");
		endpoint->tlp_dev = NULL;
	}

	if (endpoint->rep != NULL) {
		result = doca_devemu_pci_type_destroy_rep(endpoint->rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy representor (VHCA 0x%x): %s",
				     endpoint->vhca_id,
				     doca_error_get_descr(result));
			return result;
		}
		endpoint->rep = NULL;
	}

	endpoint->device_present = false;
	endpoint->vhca_id = 0;
	return DOCA_SUCCESS;
}

/**
 * Set Memory Write TLP header for MMIO operations (e.g., MSI interrupt)
 *
 * @tlp_header_buf [out]: Pointer to TLP header buffer (ACG buffer)
 * @msi_addr [in]: Target MSI address (32-bit or 64-bit)
 * @msi_data [in]: MSI data value
 * @requester_bdf [in]: Requester BDF (for requester ID field)
 * @return: Size of constructed TLP header in bytes
 */
static inline size_t set_memory_write_tlp_header(void *tlp_header_buf,
						 uint64_t msi_addr,
						 uint16_t msi_data,
						 uint16_t requester_bdf)
{
	uint32_t *header_dw = (uint32_t *)tlp_header_buf;
	bool use_64bit = (msi_addr >> 32) != 0;
	size_t header_dwords = use_64bit ? 5 : 4;

	/* DW0: fmt[31:29], type[28:24], length[9:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29),
		       use_64bit ? MEM_WR_FMT_4DW_W_DATA : MEM_WR_FMT_3DW_W_DATA,
		       &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), MEM_WR_TYPE, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), 1, &header_dw[0]); /* Length = 1 DWORD of data */

	/* DW1: requester_id[31:16], tag[15:8], last_dw_be[7:4], first_dw_be[3:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), requester_bdf, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), 0, &header_dw[1]);  /* Tag = 0 */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(7, 4), 0, &header_dw[1]);   /* Last DW BE = 0 */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(3, 0), 0xF, &header_dw[1]); /* First DW BE = 0xF (all bytes valid) */

	if (use_64bit) {
		/* 4DW header format */
		/* DW2: addr[63:32] */
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 0), (uint32_t)(msi_addr >> 32), &header_dw[2]);
		/* DW3: addr[31:2], ph[1:0] (ph already zeroed by memset) */
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 2), (uint32_t)(msi_addr >> 2), &header_dw[3]);
		/* DW4: data[31:0] (little endian for payload) */
		header_dw[4] = msi_data;
	} else {
		/* 3DW header format */
		/* DW2: addr[31:2], ph[1:0] (ph already zeroed by memset) */
		DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 2), (uint32_t)(msi_addr >> 2), &header_dw[2]);
		/* DW3: data[31:0] (little endian for payload) */
		header_dw[3] = msi_data;
	}

	return header_dwords * sizeof(uint32_t);
}

/*
 * Send MSI interrupt by constructing and sending Memory Write TLP via DOCA API
 *
 * @tlp_ctx [in]: TLP context
 * @dsp [in]: DSP bridge device
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t send_msi_via_memory_write_tlp(struct tlp_context *tlp_ctx, struct pci_device_config *dsp)
{
	/* Check if MSI is enabled */
	if (!(dsp->caps.msi.message_control & 0x0001)) {
		DOCA_LOG_DBG("MSI not enabled (Message Control=0x%04X)", dsp->caps.msi.message_control);
		return DOCA_ERROR_BAD_STATE;
	}

	/* Check if Host has configured MSI address */
	uint64_t msi_addr = ((uint64_t)dsp->caps.msi.message_address_high << 32) | dsp->caps.msi.message_address_low;
	if (msi_addr == 0) {
		DOCA_LOG_DBG("MSI address not configured by Host");
		return DOCA_ERROR_BAD_STATE;
	}

	/* Get MSI data */
	uint16_t msi_data = dsp->caps.msi.message_data;

	/* Calculate DSP's real BDF from primary_bus (updated by Host during enumeration) */
	uint16_t dsp_bdf = BDF(dsp->cfg_space_hdr.type1.primary_bus, dsp->device, dsp->function);

	DOCA_LOG_DBG("Sending MSI: addr=0x%lX, data=0x%X, requester=0x%04X", msi_addr, msi_data, dsp_bdf);

	/* Get ACG credit from queue */
	struct doca_devemu_pci_tlp_channel_req *acg_req = acg_queue_pop(tlp_ctx);
	if (acg_req == NULL) {
		DOCA_LOG_DBG("No ACG credit available, Host will poll to detect hotplug");
		return DOCA_ERROR_AGAIN;
	}

	/* Get ACG buffer to populate MMIO Write TLP */
	void *acg_buf = doca_devemu_pci_tlp_channel_req_get_acg_buf(acg_req);
	if (acg_buf == NULL) {
		DOCA_LOG_ERR("Failed to get ACG buffer from ACG request");
		doca_devemu_pci_tlp_channel_req_complete_acg(acg_req,
							     0,
							     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		return DOCA_ERROR_DRIVER;
	}

	size_t tlp_size = set_memory_write_tlp_header(acg_buf, msi_addr, msi_data, dsp_bdf);

	/* Print Memory Write TLP content for debugging */
	uint32_t *tlp_dw = (uint32_t *)acg_buf;
	bool is_64bit = (tlp_size == 20);
	DOCA_LOG_DBG("TX MemWr TLP: BDF=%04x Addr=%016lx Data=%04x [", dsp_bdf, msi_addr, msi_data);
	for (size_t i = 0; i < tlp_size / 4; i++)
		DOCA_LOG_DBG("%08x%s", DOCA_BETOH32(tlp_dw[i]), (i < tlp_size / 4 - 1) ? " " : "");
	DOCA_LOG_DBG("] %s\n", is_64bit ? "64bit" : "32bit");

	doca_devemu_pci_tlp_channel_req_complete_acg(acg_req,
						     tlp_size,
						     DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_MMIO_WRITE);
	DOCA_LOG_INFO("MSI interrupt sent successfully (TLP size: %zu bytes)", tlp_size);

	return DOCA_SUCCESS;
}

/*
 * Trigger Command Completed event and send MSI if enabled
 *
 * @tlp_ctx [in]: TLP context
 * @dsp [in/out]: DSP device configuration
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t trigger_command_completed(struct tlp_context *tlp_ctx, struct pci_device_config *dsp)
{
	/* Set Command Completed status bit */
	dsp->caps.express.slot_status |= SLOT_STS_CMD_COMPLETED;

	DOCA_LOG_DBG("Command Completed: control=0x%04X, status=0x%04X",
		     dsp->caps.express.slot_control,
		     dsp->caps.express.slot_status);

	/* Send MSI if Command Completed Interrupt is enabled */
	if (dsp->caps.express.slot_control & SLOT_CTRL_CMD_COMPLETED_INT_EN) {
		doca_error_t result = send_msi_via_memory_write_tlp(tlp_ctx, dsp);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_DBG("Failed to send Command Completed MSI: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_INFO("Command Completed MSI sent");
	}

	return DOCA_SUCCESS;
}

/*
 * Trigger hotplug event for a DSP slot
 *
 * @tlp_ctx [in]: TLP context
 * @dsp_index [in]: DSP index (0 to num_dsp-1)
 * @plug [in]: True for plug (hotplug), false for unplug (hotunplug)
 * @return: DOCA_SUCCESS on success, error otherwise
 */
static doca_error_t trigger_hotplug_event(struct tlp_context *tlp_ctx, uint32_t dsp_index, bool plug)
{
	doca_error_t result;
	struct pci_device_config *dsp;
	struct pci_device_config *endpoint;

	if (dsp_index >= tlp_ctx->num_dsp) {
		DOCA_LOG_ERR("Invalid DSP index: %u (max: %u)", dsp_index, tlp_ctx->num_dsp - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + dsp_index];
	endpoint = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + dsp_index];

	/* Hotplug requires HP interrupt enabled (polling not supported) */
	if (!(dsp->caps.express.slot_control & SLOT_CTRL_HP_INT_EN)) {
		DOCA_LOG_ERR("HP interrupt not enabled for DSP[%u], hotplug not supported", dsp_index);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* Check ACG credit availability before modifying device state */
	if (tlp_ctx->acg_queue_count == 0) {
		DOCA_LOG_DBG("No ACG credit available for MSI, discarding hotplug event");
		return DOCA_ERROR_AGAIN;
	}

	if (plug) {
		if (endpoint->device_present) {
			DOCA_LOG_WARN("DSP[%u] already has device, cannot plug again", dsp_index);
			return DOCA_ERROR_ALREADY_EXIST;
		}
		DOCA_LOG_INFO("Creating device for DSP[%u]", dsp_index);
		result = create_device(tlp_ctx, endpoint);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create device: %s", doca_error_get_descr(result));
			return result;
		}
	} else {
		if (!endpoint->device_present) {
			DOCA_LOG_ERR("DSP[%u] has no device, cannot unplug", dsp_index);
			return DOCA_ERROR_NOT_FOUND;
		}
		DOCA_LOG_INFO("Removing device from DSP[%u]", dsp_index);
		result = destroy_device(endpoint);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy device: %s", doca_error_get_descr(result));
			return result;
		}
	}

	update_dsp_slot_status(dsp, plug);

	/* Send MSI to notify Host (HP interrupt already verified as enabled) */
	result = send_msi_via_memory_write_tlp(tlp_ctx, dsp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("MSI not sent: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Hotplug: DSP[%u] %s", dsp_index, plug ? "plug" : "unplug");
	return DOCA_SUCCESS;
}

/*
 * Get PF index for a given endpoint device
 *
 * @tlp_ctx [in]: TLP context
 * @dev_cfg [in]: Device configuration
 * @pf_index [out]: Pointer to store the PF index
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t get_pf_index_for_device(struct tlp_context *tlp_ctx,
					    struct pci_device_config *dev_cfg,
					    uint32_t *pf_index)
{
	uint32_t i;

	/* Only endpoint devices have PF indices */
	if (!dev_cfg->is_endpoint)
		return DOCA_ERROR_NOT_SUPPORTED;

	/* Find device index in the devices array */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		if (dev_cfg == &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i]) {
			*pf_index = i;
			return DOCA_SUCCESS;
		}
	}

	/* Device not found in PF range */
	return DOCA_ERROR_NOT_FOUND;
}

/**
 * Get TLP request type from TLP request header buffer
 *
 * @tlp_req [in]: Pointer to TLP request
 *
 * @return: TLP request type enumeration value based on PCI specification
 */
static inline enum tlp_req_type get_tlp_req_type(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint8_t type = GET_TLP_REQ_TYPE(tlp_req_header);

	switch (type) {
	case TLP_REQ_TYPE_MEMORY_READ_WRITE:
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_MEMORY_WRITE;
		} else {
			return TLP_REQ_TYPE_MEMORY_READ;
		}
	case TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0:
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0;
		} else {
			return TLP_REQ_TYPE_CONFIG_READ_TYPE_0;
		}
	case TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1:
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1;
		} else {
			return TLP_REQ_TYPE_CONFIG_READ_TYPE_1;
		}
	default:
		return TLP_REQ_TYPE_INVALID;
	}
}

#define BDF_MAP_KEY(tlp_type, bdf) (((uint32_t)(tlp_type) << 16) | (bdf))

/*
 * Remove entry from BDF map hash chain
 *
 * @tlp_ctx [in]: TLP context
 * @entry [in]: Entry to remove
 * @old_key [in]: Key value of entry to remove
 */
static void remove_from_bdf_map(struct tlp_context *tlp_ctx, struct bdf_map_entry *entry, uint32_t old_key)
{
	uint32_t old_hash = old_key % BDF_MAP_SIZE;
	struct bdf_map_entry **pp = &tlp_ctx->bdf_map[old_hash];
	while (*pp != NULL) {
		if (*pp == entry) {
			*pp = entry->next;
			entry->next = NULL;
			return;
		}
		pp = &(*pp)->next;
	}
}

/*
 * Update BDF map with a device entry for fast lookup
 *
 * @tlp_ctx [in]: TLP context
 * @key [in]: BDF map key (includes tlp_type)
 * @dev_cfg [in]: Device pointer
 */
static void update_bdf_map(struct tlp_context *tlp_ctx, uint32_t key, struct pci_device_config *dev_cfg)
{
	uint32_t hash = key % BDF_MAP_SIZE;
	uint32_t entry_idx = dev_cfg - tlp_ctx->devs_config;
	struct bdf_map_entry *entry = &tlp_ctx->bdf_entries[entry_idx];

	/* If entry already mapped with same key, no update needed */
	if (entry->dev_cfg == dev_cfg && entry->key == key)
		return;

	/* If entry was previously mapped with different key, remove from old chain */
	if (entry->dev_cfg != NULL)
		remove_from_bdf_map(tlp_ctx, entry, entry->key);

	/* Add entry to new hash chain */
	entry->key = key;
	entry->dev_cfg = dev_cfg;
	entry->next = tlp_ctx->bdf_map[hash];
	tlp_ctx->bdf_map[hash] = entry;
}

/*
 * Find device by key using hash table for fast lookup
 *
 * @tlp_ctx [in]: TLP context
 * @key [in]: BDF map key (includes tlp_type)
 * @return: Pointer to device or NULL if not found in map
 */
static inline struct pci_device_config *lookup_device_by_key(struct tlp_context *tlp_ctx, uint32_t key)
{
	uint32_t hash = key % BDF_MAP_SIZE;
	struct bdf_map_entry *entry = tlp_ctx->bdf_map[hash];
	while (entry != NULL) {
		if (entry->key == key) {
			if (entry->dev_cfg->is_endpoint && !entry->dev_cfg->device_present) {
				return NULL;
			}
			return entry->dev_cfg;
		}
		entry = entry->next;
	}
	return NULL;
}

/*
 * Find device by BDF and TLP type, scoped to a specific NV switch TLP DSP's sub-topology (switch)
 *
 * @tlp_ctx [in]: TLP context containing device array
 * @dsp_id [in]: NV switch TLP DSP identifier that forwarded the request
 * @bus [in]: Bus number
 * @device [in]: Device number
 * @function [in]: Function number
 * @tlp_type [in]: TLP request type
 * @return: Pointer to device configuration or dummy device if not found
 */
static struct pci_device_config *find_device_by_bdf(struct tlp_context *tlp_ctx,
						    uint8_t dsp_id,
						    uint8_t bus,
						    uint8_t device,
						    uint8_t function,
						    uint8_t tlp_type)
{
	uint16_t bdf = BDF(bus, device, function);
	uint32_t key = BDF_MAP_KEY(tlp_type, bdf);
	struct pci_device_config *dev_cfg;
	uint32_t group_start;
	uint32_t group_count;
	uint32_t i;

	/* Try fast lookup in BDF map first */
	dev_cfg = lookup_device_by_key(tlp_ctx, key);
	if (dev_cfg != NULL)
		return dev_cfg;

	/* Getting here, DSP ID is expected to be a non-special value (i.e. actual DSP index) */
	if (dsp_id >= tlp_ctx->num_nv_switch_tlp_dsp) {
		DOCA_LOG_ERR("Invalid DSP ID: %u (expected: 0-%u)", dsp_id, tlp_ctx->num_nv_switch_tlp_dsp - 1);
		return &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
	}

	get_dsp_group(tlp_ctx, dsp_id, &group_start, &group_count);

	/* Slow path: find by topology rules and update map */

	/* Type 0 Config Request - USP only */
	if (tlp_type == TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0) {
		/* USP is accessed via Type 0 (device 0, function 0) */
		if (device == 0 && function == 0) {
			dev_cfg = &tlp_ctx->devs_config[USP_IDX(tlp_ctx, dsp_id)];
			update_bdf_map(tlp_ctx, key, dev_cfg);
			return dev_cfg;
		}
	}
	/* Type 1 Config Request - DSP bridges and endpoints */
	else if (tlp_type == TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1) {
		uint8_t usp_sec_bus = tlp_ctx->devs_config[USP_IDX(tlp_ctx, dsp_id)].cfg_space_hdr.type1.secondary_bus;
		uint32_t ep_base = group_start - FIRST_DSP_IDX(tlp_ctx);

		/* Check DSP bridges on this USP's secondary bus */
		if (usp_sec_bus != 0 && bus == usp_sec_bus && function == 0) {
			/* DSP devices have device numbers 0-(group_count-1) */
			for (i = 0; i < group_count; i++) {
				if (device == i) {
					dev_cfg = &tlp_ctx->devs_config[group_start + i];
					update_bdf_map(tlp_ctx, key, dev_cfg);
					return dev_cfg;
				}
			}
		}

		/* Check Single-PF endpoints on each DSP's secondary bus */
		for (i = 0; i < group_count; i++) {
			struct pci_device_config *ep_cfg = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + ep_base + i];
			uint8_t dsp_sec_bus = tlp_ctx->devs_config[group_start + i].cfg_space_hdr.type1.secondary_bus;

			if (dsp_sec_bus != 0 && bus == dsp_sec_bus && device == 0 && function == 0 &&
			    ep_cfg->device_present) {
				update_bdf_map(tlp_ctx, key, ep_cfg);
				return ep_cfg;
			}
		}
	}

	/* Return dummy device for invalid BDF */
	return &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
}

/*
 * Read PCI configuration space header register 00h (Vendor/Device ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg0(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.device_id << 16) | dev_cfg->cfg_space_hdr.type0.vendor_id;
	return value;
}

/*
 * Read PCI configuration space header register 01h (Command/Status)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg1(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.status << 16) | dev_cfg->cfg_space_hdr.type0.command;
	return value;
}

/*
 * Read PCI configuration space header register 02h (Class Code/Revision ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg2(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.class_code << 8) | dev_cfg->cfg_space_hdr.type0.revision_id;
	return value;
}

/*
 * Read PCI configuration space header register 03h (BIST/Header Type/Latency Timer/Cache Line Size)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg3(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.bist << 24) | (dev_cfg->cfg_space_hdr.type0.header_type << 16) |
		(dev_cfg->cfg_space_hdr.type0.latency_timer << 8) | dev_cfg->cfg_space_hdr.type0.cache_line_size;
	return value;
}

/*
 * Read PCI configuration space BAR register (04h to 09h)
 *
 * @dev_cfg [in]: Device configuration
 * @reg_num [in]: Register number
 * @return: BAR register value
 */
static inline uint32_t config_space_type0_read_bar(struct pci_device_config *dev_cfg, unsigned reg_num)
{
	unsigned bar_id = reg_num - 0x4;
	return dev_cfg->cfg_space_hdr.type0.bar[bar_id];
}

/*
 * Read PCI configuration space header register 0Bh (Subsystem IDs)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg11(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.subsystem_id << 16) | dev_cfg->cfg_space_hdr.type0.subsystem_vendor_id;
	return value;
}

/*
 * Read PCI configuration space header register 0Ch (Expansion ROM Base Address)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg12(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type0.exp_rom_base_addr;
}

/*
 * Read PCI configuration space header register 0Dh (Capabilities Pointer)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg13(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = dev_cfg->cfg_space_hdr.type0.cap_ptr;
	return value;
}

/*
 * Read PCI configuration space header register 0Fh (Interrupt Line/Pin)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg15(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.max_latency << 24) | (dev_cfg->cfg_space_hdr.type0.min_grant << 16) |
		(dev_cfg->cfg_space_hdr.type0.interrupt_pin << 8) | dev_cfg->cfg_space_hdr.type0.interrupt_line;
	return value;
}

/*
 * Read PCI Express capability data (matches tlp_handler config_space_cap_pcie_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within Express capability (0-14 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_pcie_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return (dev_cfg->caps.express.pcie_cap_register << 16) | (dev_cfg->caps.express.next_cap_ptr << 8) |
		       dev_cfg->caps.express.cap_id;
	case 1:
		return dev_cfg->caps.express.dev_capabilities;
	case 2:
		return (dev_cfg->caps.express.dev_status << 16) | dev_cfg->caps.express.dev_control;
	case 3:
		return dev_cfg->caps.express.link_capabilities;
	case 4:
		return (dev_cfg->caps.express.link_status << 16) | dev_cfg->caps.express.link_control;
	case 5:
		return dev_cfg->caps.express.slot_capabilities;
	case 6:
		return (dev_cfg->caps.express.slot_status << 16) | dev_cfg->caps.express.slot_control;
	case 7:
		return (dev_cfg->caps.express.root_capabilities << 16) | dev_cfg->caps.express.root_control;
	case 8:
		return dev_cfg->caps.express.root_status;
	case 9:
		return dev_cfg->caps.express.dev_capabilities2;
	case 10:
		return (dev_cfg->caps.express.dev_status2 << 16) | dev_cfg->caps.express.dev_control2;
	case 11:
		return dev_cfg->caps.express.link_capabilities2;
	case 12:
		return (dev_cfg->caps.express.link_status2 << 16) | dev_cfg->caps.express.link_control2;
	case 13:
		return dev_cfg->caps.express.slot_capabilities2;
	case 14:
		return (dev_cfg->caps.express.slot_status2 << 16) | dev_cfg->caps.express.slot_control2;
	default:
		return 0;
	}
}

/*
 * Read VPD capability data (matches tlp_handler config_space_cap_vpd_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within VPD capability (0-1 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_vpd_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return ((uint32_t)dev_cfg->caps.vpd.addr_register << 16) |
		       ((uint32_t)dev_cfg->caps.vpd.next_cap_ptr << 8) | (uint32_t)dev_cfg->caps.vpd.cap_id;
	case 1:
		return dev_cfg->caps.vpd.data_register;
	default:
		return 0;
	}
}

/*
 * Read MSI capability data
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within MSI capability (0-5 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_msi_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0: /* Cap ID + Next + Message Control */
		return (dev_cfg->caps.msi.message_control << 16) | (dev_cfg->caps.msi.next_cap_ptr << 8) |
		       dev_cfg->caps.msi.cap_id;
	case 1: /* Message Address Low */
		return dev_cfg->caps.msi.message_address_low;
	case 2: /* Message Address High */
		return dev_cfg->caps.msi.message_address_high;
	case 3: /* Message Data + Reserved */
		return (dev_cfg->caps.msi.reserved << 16) | dev_cfg->caps.msi.message_data;
	case 4: /* Mask Bits */
		return dev_cfg->caps.msi.mask_bits;
	case 5: /* Pending Bits */
		return dev_cfg->caps.msi.pending_bits;
	default:
		return 0;
	}
}

/*
 * Read MSI-X capability data (matches tlp_handler config_space_cap_msix_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within MSI-X capability (0-2 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_msix_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return (dev_cfg->caps.msix.message_control << 16) | (dev_cfg->caps.msix.next_cap_ptr << 8) |
		       dev_cfg->caps.msix.cap_id;
	case 1:
		return dev_cfg->caps.msix.table_offset;
	case 2:
		return dev_cfg->caps.msix.pba_offset;
	default:
		return 0;
	}
}

/*
 * Read PM capability data (matches tlp_handler config_space_cap_pm_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within PM capability (0-1 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_pm_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return (dev_cfg->caps.pm.pmc << 16) | (dev_cfg->caps.pm.next_cap_ptr << 8) | dev_cfg->caps.pm.cap_id;
	case 1:
		return (dev_cfg->caps.pm.data << 24) | (dev_cfg->caps.pm.reserved << 16) | dev_cfg->caps.pm.pmcsr;
	default:
		return 0;
	}
}

/*
 * Read PCI configuration space Type 1 header register 00h (Vendor/Device ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg0(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.device_id << 16) | dev_cfg->cfg_space_hdr.type1.vendor_id;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 01h (Command/Status)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg1(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.status << 16) | dev_cfg->cfg_space_hdr.type1.command;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 02h (Class Code/Revision ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg2(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.class_code << 8) | dev_cfg->cfg_space_hdr.type1.revision_id;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 03h (BIST/Header Type/Latency Timer/Cache Line Size)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg3(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.bist << 24) | (dev_cfg->cfg_space_hdr.type1.header_type << 16) |
		(dev_cfg->cfg_space_hdr.type1.latency_timer << 8) | dev_cfg->cfg_space_hdr.type1.cache_line_size;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 06h (Bus Numbers)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg6(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.secondary_latency << 24) |
		(dev_cfg->cfg_space_hdr.type1.subordinate_bus << 16) |
		(dev_cfg->cfg_space_hdr.type1.secondary_bus << 8) | dev_cfg->cfg_space_hdr.type1.primary_bus;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 07h (I/O Base/Limit)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg7(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.secondary_status << 16) | (dev_cfg->cfg_space_hdr.type1.io_limit << 8) |
		dev_cfg->cfg_space_hdr.type1.io_base;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 08h (Memory Base/Limit)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg8(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.memory_limit << 16) | dev_cfg->cfg_space_hdr.type1.memory_base;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 09h (Prefetchable Memory Base/Limit)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg9(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	/* Include 64-bit capability indicator (bits [3:0] = 0x1 for both base and limit) */
	value = ((dev_cfg->cfg_space_hdr.type1.pre_memory_limit & 0xFFF0) << 16) |
		(dev_cfg->cfg_space_hdr.type1.pre_memory_base & 0xFFF0) | 0x00010001;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 0Ah (Prefetchable Base Upper 32 Bits)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg10(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type1.pre_memory_base_upper_32bit;
}

/*
 * Read PCI configuration space Type 1 header register 0Bh (Prefetchable Limit Upper 32 Bits)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg11(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type1.pre_memory_limit_upper_32bit;
}

/*
 * Read PCI configuration space Type 1 header register 0Ch (I/O Base/Limit Upper 16 Bits)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg12(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.io_limit_upper_16bit << 16) |
		dev_cfg->cfg_space_hdr.type1.io_base_upper_16bit;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 0Dh (Capabilities Pointer)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg13(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = dev_cfg->cfg_space_hdr.type1.cap_ptr;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 0Eh (Expansion ROM Base Address)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg14(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type1.exp_rom_base_addr;
}

/*
 * Read PCI configuration space Type 1 header register 0Fh (Interrupt Line/Pin/Bridge Control)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg15(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.bridge_control << 16) |
		(dev_cfg->cfg_space_hdr.type1.interrupt_pin << 8) | dev_cfg->cfg_space_hdr.type1.interrupt_line;
	return value;
}

/*
 * Write PCI configuration space Type 0 header register 01h (Command/Status)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type0_write_reg1(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg1 {
		uint32_t as_dw;
		struct {
			uint16_t command;
			uint16_t status;
		};
	};
	union reg1 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Handle Command register bits (Read/Write) */
	if (mask.command & COMMAND_RW_MEM_SPACE_ENABLE) {
		if (write_data.command & COMMAND_RW_MEM_SPACE_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_MEM_SPACE_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_MEM_SPACE_ENABLE;
	}
	if (mask.command & COMMAND_RW_BUS_MASTER_ENABLE) {
		if (write_data.command & COMMAND_RW_BUS_MASTER_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_BUS_MASTER_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_BUS_MASTER_ENABLE;
	}
	if (mask.command & COMMAND_RW_PERR_ENABLE) {
		if (write_data.command & COMMAND_RW_PERR_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_PERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_PERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_SERR_ENABLE) {
		if (write_data.command & COMMAND_RW_SERR_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_SERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_SERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_INT_DISABLE) {
		if (write_data.command & COMMAND_RW_INT_DISABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_INT_DISABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_INT_DISABLE;
	}

	/* Handle Status register bits (Write-1-to-Clear) */
	if (mask.status & STATUS_WR1C_MASTER_DATA_PERR) {
		if (write_data.status & STATUS_WR1C_MASTER_DATA_PERR)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_MASTER_DATA_PERR;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_TA) {
		if (write_data.status & STATUS_WR1C_SIGNALED_TA)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_SIGNALED_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_TA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_TA)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_RECEIVE_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_MA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_MA)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_RECEIVE_MA;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_SERR) {
		if (write_data.status & STATUS_WR1C_SIGNALED_SERR)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_SIGNALED_SERR;
	}
	if (mask.status & STATUS_WR1C_DETECTED_PERR) {
		if (write_data.status & STATUS_WR1C_DETECTED_PERR)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_DETECTED_PERR;
	}
}

/*
 * Write PCI configuration space Type 0 header BAR registers (04h-09h)
 *
 * @dev_cfg [in/out]: Device configuration
 * @reg_num [in]: Register number (4-9)
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type0_write_bar(struct pci_device_config *dev_cfg,
						uint32_t reg_num,
						uint32_t data,
						uint32_t be_mask)
{
	uint32_t bar_idx;
	union bar_data {
		uint32_t as_dw;
	};
	union bar_data write_data, mask;

	bar_idx = reg_num - 4;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Only support BAR0 (64-bit) and BAR1 (upper 32-bits of BAR0) */
	if (bar_idx > 1)
		return;

	/* BAR sizing: when Host writes all 1s with full mask, return size mask */
	if (write_data.as_dw == 0xFFFFFFFF && mask.as_dw == 0xFFFFFFFF) {
		uint32_t bar_size = 1 << dev_cfg->log_bar_size;
		if (bar_idx == 0) {
			/* BAR0 lower 32-bits: return size mask with type bits */
			dev_cfg->cfg_space_hdr.type0.bar[0] = (~(bar_size - 1) & 0xFFFFFFFF) | BAR_ENCODING_MEM_SPACE |
							      BAR_MEM_TYPE_64_BIT | BAR_MEM_PREFETCHABLE;
			DOCA_LOG_DBG("EP BAR0 Sizing: returning 0x%08x (16KB)", dev_cfg->cfg_space_hdr.type0.bar[0]);
		} else {
			/* BAR1 upper 32-bits: return upper size mask (for 64-bit BAR) */
			dev_cfg->cfg_space_hdr.type0.bar[1] = ((uint64_t) ~(bar_size - 1) >> 32) & 0xFFFFFFFF;
			DOCA_LOG_DBG("EP BAR1 Sizing: returning 0x%08x (16KB upper)",
				     dev_cfg->cfg_space_hdr.type0.bar[1]);
		}
	} else {
		/* Normal address write: apply masked data */
		if (mask.as_dw) {
			dev_cfg->cfg_space_hdr.type0.bar[bar_idx] = write_data.as_dw & mask.as_dw;
			if (bar_idx == 0) {
				/* BAR0: Clear low 4 bits and set type bits */
				dev_cfg->cfg_space_hdr.type0.bar[0] &= 0xFFFFFFF0;
				dev_cfg->cfg_space_hdr.type0.bar[0] |= BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT |
								       BAR_MEM_PREFETCHABLE;
			}
			/* Only log complete address when BAR1 (high 32-bits) is written */
			if (bar_idx == 1) {
				uint64_t bar_addr = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
						    (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
				DOCA_LOG_INFO("EP BAR0 Final Address: 0x%lx", bar_addr);
			}
		}
	}
}

/*
 * Handle PCI configuration space header write request (Type 0 - Endpoint)
 *
 * @dev_cfg [in/out]: Device configuration
 * @reg_num [in]: Register number to write
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void handle_type0_config_write(struct pci_device_config *dev_cfg,
					     uint32_t reg_num,
					     uint32_t data,
					     uint32_t be_mask)
{
	switch (reg_num) {
	case 0x0: /* RO */
		break;
	case 0x1:
		config_space_type0_write_reg1(dev_cfg, data, be_mask);
		break;
	case 0x2: /* RO */
		break;
	case 0x3: /* RO */
		break;
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
	case 0x8:
	case 0x9:
		config_space_type0_write_bar(dev_cfg, reg_num, data, be_mask);
		break;
	case 0xa: /* Cardbus CIS Pointer not implemented */
		break;
	case 0xb: /* RO */
		break;
	case 0xc: /* Expansion ROM not implemented */
		break;
	case 0xd: /* RO */
		break;
	case 0xe: /* Reserved */
		break;
	case 0xf: /* Interrupt Line/Pin not implemented */
		break;
	default:
		break;
	}
}

/*
 * Write PCI configuration space Type 1 header register 01h (Command/Status)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg1(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg1 {
		uint32_t as_dw;
		struct {
			uint16_t command;
			uint16_t status;
		};
	};
	union reg1 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Handle Command register bits (Read/Write) */
	if (mask.command & COMMAND_RW_MEM_SPACE_ENABLE) {
		if (write_data.command & COMMAND_RW_MEM_SPACE_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_MEM_SPACE_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_MEM_SPACE_ENABLE;
	}
	if (mask.command & COMMAND_RW_BUS_MASTER_ENABLE) {
		if (write_data.command & COMMAND_RW_BUS_MASTER_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_BUS_MASTER_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_BUS_MASTER_ENABLE;
	}
	if (mask.command & COMMAND_RW_PERR_ENABLE) {
		if (write_data.command & COMMAND_RW_PERR_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_PERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_PERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_SERR_ENABLE) {
		if (write_data.command & COMMAND_RW_SERR_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_SERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_SERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_INT_DISABLE) {
		if (write_data.command & COMMAND_RW_INT_DISABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_INT_DISABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_INT_DISABLE;
	}

	/* Handle Status register bits (Write-1-to-Clear) */
	if (mask.status & STATUS_WR1C_MASTER_DATA_PERR) {
		if (write_data.status & STATUS_WR1C_MASTER_DATA_PERR)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_MASTER_DATA_PERR;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_TA) {
		if (write_data.status & STATUS_WR1C_SIGNALED_TA)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_SIGNALED_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_TA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_TA)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_RECEIVE_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_MA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_MA)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_RECEIVE_MA;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_SERR) {
		if (write_data.status & STATUS_WR1C_SIGNALED_SERR)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_SIGNALED_SERR;
	}
	if (mask.status & STATUS_WR1C_DETECTED_PERR) {
		if (write_data.status & STATUS_WR1C_DETECTED_PERR)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_DETECTED_PERR;
	}
}

/*
 * Write PCI configuration space Type 1 header register 03h (Cache Line/Latency/Header/BIST)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg3(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg3 {
		uint32_t as_dw;
		struct {
			uint8_t cache_line_size;
			uint8_t latency_timer;
			uint8_t header_type;
			uint8_t bist;
		};
	};
	union reg3 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	if (mask.cache_line_size)
		dev_cfg->cfg_space_hdr.type1.cache_line_size = write_data.cache_line_size;
	if (mask.latency_timer)
		dev_cfg->cfg_space_hdr.type1.latency_timer = write_data.latency_timer;
}

/*
 * Write PCI configuration space Type 1 header register 06h (Bus Numbers)
 * Critical for PCI enumeration - handles primary/secondary/subordinate bus configuration
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @tlp_ctx [in]: TLP context (for device identification in logs)
 */
static inline void config_space_type1_write_reg6(struct pci_device_config *dev_cfg,
						 uint32_t data,
						 uint32_t be_mask,
						 struct tlp_context *tlp_ctx)
{
	union reg6 {
		uint32_t as_dw;
		struct {
			uint8_t primary_bus;
			uint8_t secondary_bus;
			uint8_t subordinate_bus;
			uint8_t secondary_latency_timer;
		};
	};
	union reg6 write_data, mask;
	union reg6 old_val;
	write_data.as_dw = data;
	mask.as_dw = be_mask;
	old_val.primary_bus = dev_cfg->cfg_space_hdr.type1.primary_bus;
	old_val.secondary_bus = dev_cfg->cfg_space_hdr.type1.secondary_bus;
	old_val.subordinate_bus = dev_cfg->cfg_space_hdr.type1.subordinate_bus;

	(void)tlp_ctx;

	/*
	 * Handle bus number updates - Accept all values including 0xFF
	 * BIOS uses 0xFF for probe/reset, rejecting it causes enumeration failure
	 */
	if (mask.primary_bus)
		dev_cfg->cfg_space_hdr.type1.primary_bus = write_data.primary_bus;
	if (mask.secondary_bus)
		dev_cfg->cfg_space_hdr.type1.secondary_bus = write_data.secondary_bus;
	if (mask.subordinate_bus)
		dev_cfg->cfg_space_hdr.type1.subordinate_bus = write_data.subordinate_bus;
	if (mask.secondary_latency_timer)
		dev_cfg->cfg_space_hdr.type1.secondary_latency = write_data.secondary_latency_timer;

	/* Log significant bus number changes for debugging large topologies */
	if ((old_val.secondary_bus != dev_cfg->cfg_space_hdr.type1.secondary_bus &&
	     dev_cfg->cfg_space_hdr.type1.secondary_bus != 0) ||
	    (old_val.subordinate_bus != dev_cfg->cfg_space_hdr.type1.subordinate_bus &&
	     dev_cfg->cfg_space_hdr.type1.subordinate_bus != 0xFF &&
	     dev_cfg->cfg_space_hdr.type1.subordinate_bus != 0)) {
		DOCA_LOG_INFO("Bridge bus config: P=%02x S=%02x Sub=%02x (was: S=%02x Sub=%02x)",
			      dev_cfg->cfg_space_hdr.type1.primary_bus,
			      dev_cfg->cfg_space_hdr.type1.secondary_bus,
			      dev_cfg->cfg_space_hdr.type1.subordinate_bus,
			      old_val.secondary_bus,
			      old_val.subordinate_bus);
	}
}

/*
 * Write PCI configuration space Type 1 header register 08h (Memory Base/Limit)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg8(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg8 {
		uint32_t as_dw;
		struct {
			uint16_t memory_base;
			uint16_t memory_limit;
		};
	};
	union reg8 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Store address bits (bits [15:4]), lower 4 bits are reserved */
	if (mask.memory_base) {
		dev_cfg->cfg_space_hdr.type1.memory_base = write_data.memory_base & 0xFFF0;
		DOCA_LOG_DBG("Bridge Memory Base Write: 0x%04x", dev_cfg->cfg_space_hdr.type1.memory_base);
	}
	if (mask.memory_limit) {
		dev_cfg->cfg_space_hdr.type1.memory_limit = write_data.memory_limit & 0xFFF0;
		DOCA_LOG_DBG("Bridge Memory Limit Write: 0x%04x", dev_cfg->cfg_space_hdr.type1.memory_limit);
	}
}

/*
 * Write PCI configuration space Type 1 header register 09h (Prefetchable Memory Base/Limit)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg9(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg9 {
		uint32_t as_dw;
		struct {
			uint16_t pre_memory_base;
			uint16_t pre_memory_limit;
		};
	};
	union reg9 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Store address bits only, capability bits added during read */
	if (mask.pre_memory_base)
		dev_cfg->cfg_space_hdr.type1.pre_memory_base = write_data.pre_memory_base & 0xFFF0;
	if (mask.pre_memory_limit)
		dev_cfg->cfg_space_hdr.type1.pre_memory_limit = write_data.pre_memory_limit & 0xFFF0;
}

/*
 * Write PCI configuration space Type 1 header register 0Fh (Interrupt/Bridge Control)
 * Critical for BIOS - handles secondary bus reset during host reboot
 * IMPORTANT: Only specific Bridge Control bits are writable (bit 0 and bit 6)
 *            to match hardware behavior and prevent BIOS enumeration errors
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @tlp_ctx [in]: TLP context (for device identification in logs)
 */
static inline void config_space_type1_write_reg15(struct pci_device_config *dev_cfg,
						  uint32_t data,
						  uint32_t be_mask,
						  struct tlp_context *tlp_ctx)
{
	union reg15 {
		uint32_t as_dw;
		struct {
			uint8_t interrupt_line;
			uint8_t interrupt_pin;
			uint16_t bridge_control;
		};
	};
	union reg15 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	(void)tlp_ctx;

	if (mask.interrupt_line)
		dev_cfg->cfg_space_hdr.type1.interrupt_line = write_data.interrupt_line;
	if (mask.interrupt_pin)
		dev_cfg->cfg_space_hdr.type1.interrupt_pin = write_data.interrupt_pin;

	/* Only allow writing to specific Bridge Control bits (match tlp_emu behavior)
	 * Only bit 0 (Parity Error Response Enable) and bit 6 (Secondary Bus Reset) are writable
	 */
	if (mask.bridge_control) {
		uint16_t old_value = dev_cfg->cfg_space_hdr.type1.bridge_control;
		uint16_t new_value = old_value;
		uint16_t write_val = write_data.bridge_control;

		/* Update only writable bits: bit 0 and bit 6 */
		new_value = (old_value & ~0x0041) | (write_val & 0x0041);

		/* Log secondary bus reset for debugging */
		if ((write_val & 0x0040) && !(old_value & 0x0040)) {
			DOCA_LOG_DBG("BR_CTL: Secondary Bus Reset 0->1");
		}

		dev_cfg->cfg_space_hdr.type1.bridge_control = new_value;
	}
}

/*
 * Handle PCI configuration space header write request (Type 1 - Bridge)
 *
 * @dev_cfg [in/out]: Device configuration
 * @reg_num [in]: Register number to write
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @tlp_ctx [in]: TLP context
 */
static inline void handle_type1_config_write(struct pci_device_config *dev_cfg,
					     uint32_t reg_num,
					     uint32_t data,
					     uint32_t be_mask,
					     struct tlp_context *tlp_ctx)
{
	switch (reg_num) {
	case 0x0: /* RO */
		break;
	case 0x1:
		config_space_type1_write_reg1(dev_cfg, data, be_mask);
		break;
	case 0x2: /* RO */
		break;
	case 0x3:
		config_space_type1_write_reg3(dev_cfg, data, be_mask);
		break;
	case 0x4: /* BAR0 not implemented */
		break;
	case 0x5: /* BAR1 not implemented */
		break;
	case 0x6:
		config_space_type1_write_reg6(dev_cfg, data, be_mask, tlp_ctx);
		break;
	case 0x7: /* I/O Base/Limit not implemented */
		break;
	case 0x8:
		config_space_type1_write_reg8(dev_cfg, data, be_mask);
		break;
	case 0x9:
		config_space_type1_write_reg9(dev_cfg, data, be_mask);
		break;
	case 0xa:
		if (be_mask)
			dev_cfg->cfg_space_hdr.type1.pre_memory_base_upper_32bit = data;
		break;
	case 0xb:
		if (be_mask)
			dev_cfg->cfg_space_hdr.type1.pre_memory_limit_upper_32bit = data;
		break;
	case 0xc: /* I/O Base/Limit Upper not implemented */
		break;
	case 0xd: /* RO */
		break;
	case 0xe: /* Expansion ROM not implemented */
		break;
	case 0xf:
		config_space_type1_write_reg15(dev_cfg, data, be_mask, tlp_ctx);
		break;
	default:
		break;
	}
}

/**
 * Set TLP request completion header context
 *
 * @tlp_response_header [out]: Pointer to completion header buffer
 * @dev_cfg [in]: Pointer to device configuration structure containing completion context
 * @byte_count [in]: Byte count for the completion header
 */
static inline void set_tlp_req_completion_header(void *tlp_response_header,
						 struct pci_device_config *dev_cfg,
						 uint16_t byte_count)
{
	uint32_t *header_dw = (uint32_t *)tlp_response_header;

	memset(tlp_response_header, 0, 3 * sizeof(uint32_t));

	/* DW0: fmt[31:29], type[28:24], tag9[bit 23], tag8[bit 19], length[9:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), dev_cfg->cmpl_fmt, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), dev_cfg->cmpl_type, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(23, 23), dev_cfg->tag9, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(19, 19), dev_cfg->tag8, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), dev_cfg->cmpl_length, &header_dw[0]);

	/* DW1: completer_id[31:16], cmpl_status[15:13], byte_cnt[11:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), dev_cfg->completer_id, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 13), dev_cfg->cmpl_status, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(11, 0), byte_count, &header_dw[1]);

	/* DW2: requester_id[31:16], tag[15:8] (set to 0), lower_addr[6:0] (set to 0) */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), dev_cfg->requester_id, &header_dw[2]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), dev_cfg->tag, &header_dw[2]);
	/* lower_addr are already zeroed by memset */
}

/*
 * Handle PCI configuration space header read request (Type 0)
 *
 * @reg_num [in]: Register number to read
 * @dev_cfg [in]: Device configuration
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_header_read_type0(uint32_t reg_num, struct pci_device_config *dev_cfg)
{
	uint32_t data;

	switch (reg_num) {
	case 0x0:
		data = config_space_type0_read_reg0(dev_cfg);
		break;
	case 0x1:
		data = config_space_type0_read_reg1(dev_cfg);
		break;
	case 0x2:
		data = config_space_type0_read_reg2(dev_cfg);
		break;
	case 0x3:
		data = config_space_type0_read_reg3(dev_cfg);
		break;
	case 0x4 ... 0x9:
		data = config_space_type0_read_bar(dev_cfg, reg_num);
		break;
	case 0xa:
		data = 0;
		break;
	case 0xb:
		data = config_space_type0_read_reg11(dev_cfg);
		break;
	case 0xc:
		data = config_space_type0_read_reg12(dev_cfg);
		break;
	case 0xd:
		data = config_space_type0_read_reg13(dev_cfg);
		break;
	case 0xe:
		data = 0;
		break;
	case 0xf:
		data = config_space_type0_read_reg15(dev_cfg);
		break;
	default:
		data = 0;
		break;
	}

	return data;
}

/*
 * Handle PCI configuration space header read request (Type 1)
 *
 * @reg_num [in]: Register number to read
 * @dev_cfg [in]: Device configuration
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_header_read_type1(uint32_t reg_num, struct pci_device_config *dev_cfg)
{
	uint32_t data;

	switch (reg_num) {
	case 0x0:
		data = config_space_type1_read_reg0(dev_cfg);
		break;
	case 0x1:
		data = config_space_type1_read_reg1(dev_cfg);
		break;
	case 0x2:
		data = config_space_type1_read_reg2(dev_cfg);
		break;
	case 0x3:
		data = config_space_type1_read_reg3(dev_cfg);
		break;
	case 0x4:
		data = 0;
		break;
	case 0x5:
		data = 0;
		break;
	case 0x6:
		data = config_space_type1_read_reg6(dev_cfg);
		break;
	case 0x7:
		data = config_space_type1_read_reg7(dev_cfg);
		break;
	case 0x8:
		data = config_space_type1_read_reg8(dev_cfg);
		break;
	case 0x9:
		data = config_space_type1_read_reg9(dev_cfg);
		break;
	case 0xa:
		data = config_space_type1_read_reg10(dev_cfg);
		break;
	case 0xb:
		data = config_space_type1_read_reg11(dev_cfg);
		break;
	case 0xc:
		data = config_space_type1_read_reg12(dev_cfg);
		break;
	case 0xd:
		data = config_space_type1_read_reg13(dev_cfg);
		break;
	case 0xe:
		data = config_space_type1_read_reg14(dev_cfg);
		break;
	case 0xf:
		data = config_space_type1_read_reg15(dev_cfg);
		break;
	default:
		data = 0;
		break;
	}

	return data;
}

/*
 * Handle PCI configuration space capabilities read request
 *
 * @reg_num [in]: Register number to read
 * @dev_cfg [in]: Device configuration
 * @cap_id [out]: Pointer to capability ID to be filled
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_caps_read(uint32_t reg_num,
						     struct pci_device_config *dev_cfg,
						     uint16_t *cap_id)
{
	uint32_t cfg_read_data = 0;

	if (reg_num >= TLP_CAP_EXPRESS_REG_NUM && reg_num < TLP_CAP_EXPRESS_REG_NUM + TLP_CAP_EXPRESS_LEN_DW) {
		cfg_read_data = config_space_cap_pcie_read(dev_cfg, reg_num - TLP_CAP_EXPRESS_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_EXPRESS;
	} else if (reg_num >= TLP_CAP_VPD_REG_NUM && reg_num < TLP_CAP_VPD_REG_NUM + TLP_CAP_VPD_LEN_DW) {
		cfg_read_data = config_space_cap_vpd_read(dev_cfg, reg_num - TLP_CAP_VPD_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_VPD;
	} else if (reg_num >= TLP_CAP_MSIX_REG_NUM && reg_num < TLP_CAP_MSIX_REG_NUM + TLP_CAP_MSIX_LEN_DW) {
		cfg_read_data = config_space_cap_msix_read(dev_cfg, reg_num - TLP_CAP_MSIX_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_MSIX;
	} else if (reg_num >= TLP_CAP_MSI_REG_NUM && reg_num < TLP_CAP_MSI_REG_NUM + TLP_CAP_MSI_LEN_DW) {
		cfg_read_data = config_space_cap_msi_read(dev_cfg, reg_num - TLP_CAP_MSI_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_MSI;
	} else if (reg_num >= TLP_CAP_PM_REG_NUM && reg_num < TLP_CAP_PM_REG_NUM + TLP_CAP_PM_LEN_DW) {
		cfg_read_data = config_space_cap_pm_read(dev_cfg, reg_num - TLP_CAP_PM_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_PM;
	} else {
		cfg_read_data = 0x00000000;
	}

	return cfg_read_data;
}

/*
 * Handle TLP request read type_0
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_read_type_0(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct pci_device_config *dev_cfg,
					      struct tlp_context *tlp_ctx,
					      uint32_t ext_reg_num,
					      uint16_t *cap_id,
					      bool *is_cap_id_valid,
					      bool *is_pcie_cap)
{
	uint32_t cfg_read_data = 0;
	(void)tlp_ctx;

	DOCA_LOG_DBG("Handle TLP Request for Config Read Type 0, ext_reg_num: %d", ext_reg_num);
	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			cfg_read_data = handle_config_space_header_read_type1(ext_reg_num, dev_cfg);
		else
			cfg_read_data = handle_config_space_header_read_type0(ext_reg_num, dev_cfg);
		break;
	case 16 ... 63:
		/* Capabilities - for endpoints and bridges (DSP with hotplug) */
		cfg_read_data = handle_config_space_caps_read(ext_reg_num, dev_cfg, cap_id);
		*is_cap_id_valid = true;
		*is_pcie_cap = false;
		break;
	default:
		DOCA_LOG_DBG("Invalid read ext_reg_num %d", ext_reg_num);
		break;
	}

	DOCA_LOG_DBG(" --> cfg_read_data:  %08x", cfg_read_data);
	/* Set the completion data */
	void *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(tlp_cpl_data, &cfg_read_data, sizeof(cfg_read_data));
	/* Set completion context */
	dev_cfg->cmpl_fmt = TLP_FMT_CPL_W_DATA;
	dev_cfg->cmpl_type = TLP_TYPE_COMPLETION;
	dev_cfg->cmpl_length = 1;
	dev_cfg->cmpl_status = TLP_CPL_STATUS_SC;
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      dev_cfg,
				      BYTES_IN_DWORD);
	doca_devemu_pci_tlp_channel_req_complete_config_read(tlp_req,
							     dev_cfg->tlp_dev,
							     (uint8_t)*is_cap_id_valid,
							     *cap_id,
							     (uint8_t)*is_pcie_cap);
	(void)tlp_ctx;
}

/*
 * Handle TLP request read type_1
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_read_type_1(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct pci_device_config *dev_cfg,
					      struct tlp_context *tlp_ctx,
					      uint32_t ext_reg_num,
					      uint16_t *cap_id,
					      bool *is_cap_id_valid,
					      bool *is_pcie_cap)
{
	uint32_t cfg_read_data = 0;
	(void)tlp_ctx;

	DOCA_LOG_DBG("Handle TLP Request for Config Read Type 1, ext_reg_num: %d", ext_reg_num);

	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			cfg_read_data = handle_config_space_header_read_type1(ext_reg_num, dev_cfg);
		else
			cfg_read_data = handle_config_space_header_read_type0(ext_reg_num, dev_cfg);
		break;
	case 16 ... 63:
		/* Capabilities - for endpoints and bridges (DSP with hotplug) */
		cfg_read_data = handle_config_space_caps_read(ext_reg_num, dev_cfg, cap_id);
		*is_cap_id_valid = true;
		*is_pcie_cap = false;
		break;
	default:
		DOCA_LOG_DBG("Invalid read ext_reg_num %d", ext_reg_num);
		break;
	}

	DOCA_LOG_DBG(" --> cfg_read_data:  %08x", cfg_read_data);
	/* Set the completion data */
	void *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(tlp_cpl_data, &cfg_read_data, sizeof(cfg_read_data));
	/* Set completion context */
	dev_cfg->cmpl_fmt = TLP_FMT_CPL_W_DATA;
	dev_cfg->cmpl_type = TLP_TYPE_COMPLETION;
	dev_cfg->cmpl_length = 1;
	dev_cfg->cmpl_status = TLP_CPL_STATUS_SC;
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      dev_cfg,
				      BYTES_IN_DWORD);
	doca_devemu_pci_tlp_channel_req_complete_config_read(tlp_req,
							     dev_cfg->tlp_dev,
							     (uint8_t)*is_cap_id_valid,
							     *cap_id,
							     (uint8_t)*is_pcie_cap);
	(void)tlp_ctx;
}

/*
 * Map TLP first DW byte enable to byte mask
 * Matches dpu sample (devemu_pci_device_tlp_handler) implementation
 *
 * @first_dw_be [in]: First DW byte enable (4 bits)
 * @return: Corresponding byte mask based on PCI specification
 */
static inline uint32_t map_first_dw_be_to_mask(uint32_t first_dw_be)
{
	static const uint32_t first_dw_be_to_mask[] = {
		0x00000000, /* 0000 */
		0x000000FF, /* 0001 */
		0x0000FF00, /* 0010 */
		0x0000FFFF, /* 0011 */
		0x00FF0000, /* 0100 */
		0x00FF00FF, /* 0101 */
		0x00FFFF00, /* 0110 */
		0x00FFFFFF, /* 0111 */
		0xFF000000, /* 1000 */
		0xFF0000FF, /* 1001 */
		0xFF00FF00, /* 1010 */
		0xFF00FFFF, /* 1011 */
		0xFFFF0000, /* 1100 */
		0xFFFF00FF, /* 1101 */
		0xFFFFFF00, /* 1110 */
		0xFFFFFFFF, /* 1111 */
	};

	return first_dw_be_to_mask[first_dw_be & 0xF];
}

/*
 * Write MSI capability data
 *
 * @cap_offset [in]: Offset within MSI capability (0-5 DWORDs)
 * @write_data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @dev_cfg [in/out]: Device configuration
 */
static inline void config_space_cap_msi_write(uint32_t cap_offset,
					      uint32_t write_data,
					      uint32_t be_mask,
					      struct pci_device_config *dev_cfg)
{
	switch (cap_offset) {
	case 0: /* Message Control */
		if (be_mask & 0xFFFF0000) {
			dev_cfg->caps.msi.message_control = (write_data >> 16) & 0xFFFF;
			DOCA_LOG_DBG("MSI Control: 0x%04X", dev_cfg->caps.msi.message_control);
		}
		break;
	case 1: /* Message Address Low */
		dev_cfg->caps.msi.message_address_low = write_data;
		DOCA_LOG_DBG("MSI_ADDR_LOW: 0x%08X", write_data);
		break;
	case 2: /* Message Address High */
		dev_cfg->caps.msi.message_address_high = write_data;
		DOCA_LOG_DBG("MSI_ADDR_HIGH: 0x%08X", write_data);
		DOCA_LOG_DBG("MSI_FULL_ADDR: 0x%016lX",
			     ((uint64_t)dev_cfg->caps.msi.message_address_high << 32) |
				     dev_cfg->caps.msi.message_address_low);
		break;
	case 3: /* Message Data (lower 16 bits) + Reserved (upper 16 bits) */
		DOCA_LOG_DBG("MSI_DATA_RAW: write_data=0x%08X, be_mask=0x%02X", write_data, be_mask);
		if (be_mask & 0x03) { /* Bytes 0-1 = Message Data */
			dev_cfg->caps.msi.message_data = write_data & 0xFFFF;
			DOCA_LOG_DBG("MSI_DATA: 0x%04X (vector=%u)",
				     dev_cfg->caps.msi.message_data,
				     dev_cfg->caps.msi.message_data & 0xFF);
		}
		break;
	case 4: /* Mask Bits */
		dev_cfg->caps.msi.mask_bits = write_data;
		DOCA_LOG_INFO("MSI Mask Bits: 0x%08X", dev_cfg->caps.msi.mask_bits);
		break;
	case 5: /* Pending Bits (read-only) */
		/* Pending bits are read-only, no write operation */
		break;
	default:
		break;
	}
}

/*
 * Write PCIe Express capability data
 *
 * @cap_offset [in]: Offset within Express capability (0-15 DWORDs)
 * @write_data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @dev_cfg [in/out]: Device configuration
 * @return: True if Command Completed should be triggered, false otherwise
 */
static inline bool config_space_cap_pcie_write(uint32_t cap_offset,
					       uint32_t write_data,
					       uint32_t be_mask,
					       struct pci_device_config *dev_cfg)
{
	bool trigger_cmd_completed = false;

	switch (cap_offset) {
	case 6: /* Slot Control + Slot Status (Offset 0x18 in Express Cap) */
		/* Handle Slot Control (lower 16 bits) */
		if (be_mask & 0x0000FFFF) {
			uint16_t old_control = dev_cfg->caps.express.slot_control;
			uint16_t new_control = write_data & 0xFFFF;

			/* Host sets enable bits via pcie_enable_notification(), accept as-is */
			dev_cfg->caps.express.slot_control = new_control;

			if (dev_cfg->is_bridge) {
				/* Check if device is present (PDS bit in Slot Status) */
				bool device_present =
					(dev_cfg->caps.express.slot_status & SLOT_STS_PRESENCE_DETECT_STATE) != 0;

				/*
				 * Detect Power Controller Control state change (Bit 10)
				 * PCIe Spec: bit10=0 means Power ON, bit10=1 means Power OFF
				 */
				bool old_power_off = (old_control & SLOT_CTRL_POWER_CONTROLLER) != 0;
				bool new_power_off = (new_control & SLOT_CTRL_POWER_CONTROLLER) != 0;

				if (old_power_off && !new_power_off && device_present) {
					/* Power OFF -> ON with device present: activate Data Link Layer */
					dev_cfg->caps.express.link_status |= LINK_STS_DL_ACTIVE;
					DOCA_LOG_INFO("Power ON (bit10: 1->0): DLActive set, link_status=0x%04X",
						      dev_cfg->caps.express.link_status);
				} else if (!old_power_off && new_power_off) {
					/* Power ON -> OFF: deactivate Data Link Layer */
					dev_cfg->caps.express.link_status &= ~LINK_STS_DL_ACTIVE;
					DOCA_LOG_INFO("Power OFF (bit10: 0->1): DLActive cleared, link_status=0x%04X",
						      dev_cfg->caps.express.link_status);
				}

				/* Trigger Command Completed for any Slot Control write */
				trigger_cmd_completed = true;
			}
		}

		/* Handle Slot Status (upper 16 bits) - Write-1-to-Clear bits */
		if (be_mask & 0xFFFF0000) {
			uint16_t status_w1c = (write_data >> 16) & 0xFFFF;
			/* Clear bits that are written as 1 (W1C behavior) */
			dev_cfg->caps.express.slot_status &= ~status_w1c;
			DOCA_LOG_DBG("Slot Status W1C: cleared bits 0x%04X, new status=0x%04X",
				     status_w1c,
				     dev_cfg->caps.express.slot_status);
		}
		break;

	case 2: /* Device Control + Device Status */
		/* Handle Device Control (lower 16 bits) - if needed */
		if (be_mask & 0x0000FFFF) {
			dev_cfg->caps.express.dev_control = write_data & 0xFFFF;
			DOCA_LOG_DBG("Device Control write: 0x%04X", dev_cfg->caps.express.dev_control);
		}
		/* Device Status (upper 16 bits) is mostly RO or W1C */
		break;

	case 4: /* Link Control + Link Status */
		/* Handle Link Control (lower 16 bits) - if needed */
		if (be_mask & 0x0000FFFF) {
			dev_cfg->caps.express.link_control = write_data & 0xFFFF;
			DOCA_LOG_DBG("Link Control write: 0x%04X", dev_cfg->caps.express.link_control);
		}
		/* Link Status (upper 16 bits) is mostly RO */
		break;

	default:
		/* Other Express capability registers are mostly RO */
		DOCA_LOG_DBG("Express cap write to offset %u (ignored)", cap_offset);
		break;
	}

	return trigger_cmd_completed;
}

/*
 * Handle PCI configuration space capabilities write request
 *
 * @reg_num [in]: Register number
 * @cfg_write_data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @dev_cfg [in]: Device configuration
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 * @return: True if Command Completed should be triggered, false otherwise
 */
static inline bool handle_config_space_caps_write(uint32_t reg_num,
						  uint32_t cfg_write_data,
						  uint32_t be_mask,
						  struct pci_device_config *dev_cfg,
						  uint16_t *cap_id,
						  bool *is_pcie_cap)
{
	bool trigger_cmd_completed = false;

	/* Determine capability ID based on offset and perform write */
	if (reg_num >= TLP_CAP_EXPRESS_REG_NUM && reg_num < TLP_CAP_EXPRESS_REG_NUM + TLP_CAP_EXPRESS_LEN_DW) {
		trigger_cmd_completed = config_space_cap_pcie_write(reg_num - TLP_CAP_EXPRESS_REG_NUM,
								    cfg_write_data,
								    be_mask,
								    dev_cfg);
		*cap_id = TLP_PCI_CAP_ID_EXPRESS;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_VPD_REG_NUM && reg_num < TLP_CAP_VPD_REG_NUM + TLP_CAP_VPD_LEN_DW) {
		*cap_id = TLP_PCI_CAP_ID_VPD;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_MSIX_REG_NUM && reg_num < TLP_CAP_MSIX_REG_NUM + TLP_CAP_MSIX_LEN_DW) {
		*cap_id = TLP_PCI_CAP_ID_MSIX;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_MSI_REG_NUM && reg_num < TLP_CAP_MSI_REG_NUM + TLP_CAP_MSI_LEN_DW) {
		config_space_cap_msi_write(reg_num - TLP_CAP_MSI_REG_NUM, cfg_write_data, be_mask, dev_cfg);
		*cap_id = TLP_PCI_CAP_ID_MSI;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_PM_REG_NUM && reg_num < TLP_CAP_PM_REG_NUM + TLP_CAP_PM_LEN_DW) {
		*cap_id = TLP_PCI_CAP_ID_PM;
		*is_pcie_cap = false;
	}

	return trigger_cmd_completed;
}

/*
 * Handle TLP request write type_0
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @tlp_req_header [in]: Pointer to TLP request header
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_write_type_0(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					       struct pci_device_config *dev_cfg,
					       struct tlp_context *tlp_ctx,
					       uint32_t ext_reg_num,
					       const void *tlp_req_header,
					       uint16_t *cap_id,
					       bool *is_cap_id_valid,
					       bool *is_pcie_cap)
{
	unsigned be_mask = map_first_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	const uint32_t *cfg_write_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);
	uint32_t write_data = 0;

	if (cfg_write_data != NULL)
		write_data = cfg_write_data[0];

	DOCA_LOG_DBG("Handle TLP Request for Config Write Type 0, ext_reg_num: %d", ext_reg_num);
	DOCA_LOG_DBG(" --> cfg_write_data: %08x, be_mask: %08x", write_data, be_mask);

	bool trigger_cmd_completed = false;

	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			handle_type1_config_write(dev_cfg, ext_reg_num, write_data, be_mask, tlp_ctx);
		else
			handle_type0_config_write(dev_cfg, ext_reg_num, write_data, be_mask);
		break;
	case 16 ... 63:
		/* Capabilities - for endpoints and bridges (DSP with hotplug) */
		trigger_cmd_completed =
			handle_config_space_caps_write(ext_reg_num, write_data, be_mask, dev_cfg, cap_id, is_pcie_cap);
		*is_cap_id_valid = true;
		break;
	default:
		DOCA_LOG_DBG("Invalid write ext_reg_num %d", ext_reg_num);
		break;
	}

	/* Set completion context */
	dev_cfg->cmpl_fmt = TLP_FMT_CPL_NODATA;
	dev_cfg->cmpl_type = TLP_TYPE_COMPLETION;
	dev_cfg->cmpl_length = 0;
	dev_cfg->cmpl_status = TLP_CPL_STATUS_SC;
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      dev_cfg,
				      BYTES_IN_DWORD);
	doca_devemu_pci_tlp_channel_req_complete_config_write(tlp_req,
							      dev_cfg->tlp_dev,
							      (uint8_t)*is_cap_id_valid,
							      *cap_id,
							      (uint8_t)*is_pcie_cap);

	/* Trigger Command Completed if needed (for Slot Control writes) */
	if (trigger_cmd_completed)
		trigger_command_completed(tlp_ctx, dev_cfg);

	(void)tlp_ctx;
}

/*
 * Handle TLP request write type_1
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @tlp_req_header [in]: Pointer to TLP request header
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_write_type_1(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					       struct pci_device_config *dev_cfg,
					       struct tlp_context *tlp_ctx,
					       uint32_t ext_reg_num,
					       const void *tlp_req_header,
					       uint16_t *cap_id,
					       bool *is_cap_id_valid,
					       bool *is_pcie_cap)
{
	unsigned be_mask = map_first_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	const uint32_t *cfg_write_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);
	uint32_t write_data = 0;

	if (cfg_write_data != NULL)
		write_data = cfg_write_data[0];

	DOCA_LOG_DBG("Handle TLP Request for Config Write Type 1, ext_reg_num: %d", ext_reg_num);
	DOCA_LOG_DBG(" --> cfg_write_data: %08x, be_mask: %08x", write_data, be_mask);

	bool trigger_cmd_completed = false;

	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			handle_type1_config_write(dev_cfg, ext_reg_num, write_data, be_mask, tlp_ctx);
		else
			handle_type0_config_write(dev_cfg, ext_reg_num, write_data, be_mask);
		break;
	case 16 ... 63:
		/* Capabilities - for endpoints and bridges (DSP with hotplug) */
		trigger_cmd_completed =
			handle_config_space_caps_write(ext_reg_num, write_data, be_mask, dev_cfg, cap_id, is_pcie_cap);
		*is_cap_id_valid = true;
		break;
	default:
		DOCA_LOG_DBG("Invalid write ext_reg_num %d", ext_reg_num);
		break;
	}

	/* Set completion context */
	dev_cfg->cmpl_fmt = TLP_FMT_CPL_NODATA;
	dev_cfg->cmpl_type = TLP_TYPE_COMPLETION;
	dev_cfg->cmpl_length = 0;
	dev_cfg->cmpl_status = TLP_CPL_STATUS_SC;
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      dev_cfg,
				      BYTES_IN_DWORD);
	doca_devemu_pci_tlp_channel_req_complete_config_write(tlp_req,
							      dev_cfg->tlp_dev,
							      (uint8_t)*is_cap_id_valid,
							      *cap_id,
							      (uint8_t)*is_pcie_cap);

	/* Trigger Command Completed if needed (for Slot Control writes) */
	if (trigger_cmd_completed)
		trigger_command_completed(tlp_ctx, dev_cfg);

	(void)tlp_ctx;
}

/*
 * Helper function to determine byte enable mask for a specific DWORD
 *
 * @dw_idx [in]: DWORD index
 * @num_dwords [in]: Total number of DWORDs
 * @first_dw_be [in]: First DWORD byte enable
 * @last_dw_be [in]: Last DWORD byte enable
 * @return: Byte enable mask for the specified DWORD
 */
static inline unsigned get_dword_byte_enable_mask(unsigned dw_idx,
						  unsigned num_dwords,
						  unsigned first_dw_be,
						  unsigned last_dw_be)
{
	if (num_dwords == 1 || dw_idx == 0) {
		return first_dw_be;
	} else if (dw_idx == num_dwords - 1) {
		return last_dw_be;
	} else {
		return 0xF; /* All bytes enabled */
	}
}

/*
 * Handle TLP memory read request (MMIO Read)
 * Returns dummy data for testing purposes
 *
 * @tlp_req [in]: TLP request
 * @dev_cfg [in]: Target device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_req_header [in]: TLP request header
 */
static inline void handle_tlp_req_memory_read(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct pci_device_config *dev_cfg,
					      struct tlp_context *tlp_ctx,
					      const void *tlp_req_header)
{
	uint16_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint64_t address;
	void *cpl_header = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);
	void *cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	uint32_t *data_array = (uint32_t *)cpl_data;
	unsigned num_dwords = (length == 0) ? 1024 : length;

	DOCA_LOG_DBG("Handle TLP Request for Memory Read");

	/* Parse address based on format */
	if (fmt == TLP_FMT_4DW_NODATA) {
		uint32_t addr_high = GET_MEM_ADDR_64BIT_HIGH(tlp_req_header);
		uint32_t addr_low = GET_MEM_ADDR_64BIT_LOW(tlp_req_header);
		address = ((uint64_t)addr_high << 32) | (addr_low << 2);
	} else {
		address = GET_MEM_ADDR_32BIT(tlp_req_header);
	}

	uint16_t first_dw_be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	uint16_t last_dw_be = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);

	DOCA_LOG_DBG("Memory Read: addr=0x%lx, len=%d DWs, first_be=0x%x, last_be=0x%x",
		     address,
		     num_dwords,
		     first_dw_be,
		     last_dw_be);

	/* Get BAR base address */
	uint64_t bar0_base = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
			     (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
	uint64_t offset_in_bar = address - bar0_base;

	/* Transaction region starts at offset 0x3000 within BAR0 */
	const uint64_t transaction_region_start = dev_cfg->transaction_region_start;
	const uint64_t transaction_region_end = transaction_region_start + tlp_ctx->transaction_region_size;

	/* Get PF index for this device to access its independent transaction region */
	uint32_t pf_index;
	doca_error_t ret = get_pf_index_for_device(tlp_ctx, dev_cfg, &pf_index);

	/* Read from transaction region if available and address in range */
	if (ret == DOCA_SUCCESS && tlp_ctx->transaction_region_memories != NULL &&
	    tlp_ctx->transaction_region_memories[pf_index] != NULL && offset_in_bar >= transaction_region_start &&
	    offset_in_bar < transaction_region_end) {
		uint64_t offset_in_region = offset_in_bar - transaction_region_start;
		uint8_t *region_ptr = (uint8_t *)tlp_ctx->transaction_region_memories[pf_index] + offset_in_region;
		for (unsigned dw_idx = 0; dw_idx < num_dwords && dw_idx < 256; dw_idx++) {
			uint8_t *dword_ptr = region_ptr + (dw_idx * 4);
			uint32_t dword_data = 0;
			uint8_t *data_ptr = (uint8_t *)&dword_data;
			unsigned be_mask = get_dword_byte_enable_mask(dw_idx, num_dwords, first_dw_be, last_dw_be);
			for (unsigned byte_idx = 0; byte_idx < 4; byte_idx++) {
				if (be_mask & (1 << byte_idx)) {
					data_ptr[byte_idx] = dword_ptr[byte_idx];
				}
			}
			data_array[dw_idx] = dword_data;
			DOCA_LOG_DBG("  DW[%d]: data=0x%08x, be_mask=0x%x", dw_idx, dword_data, be_mask);
		}
		DOCA_LOG_DBG("  Read from transaction region PF%d at offset 0x%lx (in BAR: 0x%lx)",
			     pf_index,
			     offset_in_region,
			     offset_in_bar);
	} else {
		for (unsigned i = 0; i < num_dwords && i < 256; i++) {
			data_array[i] = DUMMY_READ_DATA_BASE + i;
		}
		DOCA_LOG_DBG("  Using dummy data (offset 0x%lx outside transaction region 0x%lx-0x%lx)",
			     offset_in_bar,
			     transaction_region_start,
			     transaction_region_end - 1);
	}

	dev_cfg->cmpl_fmt = TLP_FMT_CPL_W_DATA;
	dev_cfg->cmpl_type = TLP_TYPE_COMPLETION;
	dev_cfg->cmpl_length = num_dwords;
	dev_cfg->cmpl_status = TLP_CPL_STATUS_SC;
	set_tlp_req_completion_header(cpl_header, dev_cfg, num_dwords * BYTES_IN_DWORD);
	DOCA_LOG_DBG("Memory Read: addr=0x%lx, len=%d DWs, cmpl_len=%d", address, length, dev_cfg->cmpl_length);
}

/*
 * Handle TLP memory write request (MMIO Write)
 *
 * @tlp_req [in]: TLP request
 * @dev_cfg [in]: Target device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_req_header [in]: TLP request header
 */
static inline void handle_tlp_req_memory_write(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					       struct pci_device_config *dev_cfg,
					       struct tlp_context *tlp_ctx,
					       const void *tlp_req_header)
{
	unsigned length = GET_TLP_REQ_LENGTH(tlp_req_header);
	unsigned first_dw_be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	unsigned last_dw_be = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint64_t address;
	unsigned num_dwords = (length == 0) ? 1024 : length;

	DOCA_LOG_DBG("Handle TLP Request for Memory Write");

	/* Parse address based on format */
	if (fmt == TLP_FMT_4DW_W_DATA) {
		uint32_t addr_high = GET_MEM_ADDR_64BIT_HIGH(tlp_req_header);
		uint32_t addr_low = GET_MEM_ADDR_64BIT_LOW(tlp_req_header);
		address = ((uint64_t)addr_high << 32) | (addr_low << 2);
	} else {
		address = GET_MEM_ADDR_32BIT(tlp_req_header);
	}

	const void *req_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);

	DOCA_LOG_DBG("Memory Write: addr=0x%lx, len=%d DWs, first_be=0x%x, last_be=0x%x",
		     address,
		     num_dwords,
		     first_dw_be,
		     last_dw_be);

	/* Get BAR base address */
	uint64_t bar0_base = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
			     (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
	uint64_t offset_in_bar = address - bar0_base;

	const uint64_t transaction_region_start = dev_cfg->transaction_region_start;
	const uint64_t transaction_region_end = transaction_region_start + tlp_ctx->transaction_region_size;

	/* Get PF index for this device to access its independent transaction region */
	uint32_t pf_index;
	doca_error_t ret = get_pf_index_for_device(tlp_ctx, dev_cfg, &pf_index);

	/* Write to transaction region if available and address in range */
	if (ret == DOCA_SUCCESS && req_data != NULL && tlp_ctx->transaction_region_memories != NULL &&
	    tlp_ctx->transaction_region_memories[pf_index] != NULL && offset_in_bar >= transaction_region_start &&
	    offset_in_bar < transaction_region_end) {
		uint64_t offset_in_region = offset_in_bar - transaction_region_start;
		uint8_t *region_ptr = (uint8_t *)tlp_ctx->transaction_region_memories[pf_index] + offset_in_region;
		const uint32_t *data_array = (const uint32_t *)req_data;
		for (unsigned dw_idx = 0; dw_idx < num_dwords; dw_idx++) {
			uint32_t dword_data = data_array[dw_idx];
			uint8_t *dword_ptr = region_ptr + (dw_idx * 4);
			uint8_t *data_ptr = (uint8_t *)&dword_data;
			unsigned be_mask = get_dword_byte_enable_mask(dw_idx, num_dwords, first_dw_be, last_dw_be);
			for (unsigned byte_idx = 0; byte_idx < 4; byte_idx++) {
				if (be_mask & (1 << byte_idx)) {
					dword_ptr[byte_idx] = data_ptr[byte_idx];
				}
			}
			DOCA_LOG_DBG("  DW[%d]: data=0x%08x, be_mask=0x%x", dw_idx, dword_data, be_mask);
		}
		DOCA_LOG_DBG("  Wrote to transaction region PF%d at offset 0x%lx (in BAR: 0x%lx)",
			     pf_index,
			     offset_in_region,
			     offset_in_bar);
	} else {
		DOCA_LOG_DBG("  Write ignored (offset 0x%lx outside transaction region 0x%lx-0x%lx)",
			     offset_in_bar,
			     transaction_region_start,
			     transaction_region_end - 1);
	}

	/* Memory write is posted - set completion context to 0 */
	dev_cfg->cmpl_fmt = 0;
	dev_cfg->cmpl_length = 0;
	dev_cfg->cmpl_status = 0;
	DOCA_LOG_DBG("Memory Write: addr=0x%lx, len=%d DWs", address, length);
}

/*
 * Find device by BAR address for Memory TLP requests
 *
 * @tlp_ctx [in]: TLP context
 * @address [in]: Memory address from TLP
 * @return: Pointer to device or dummy device if not found
 */
static struct pci_device_config *find_device_by_address(struct tlp_context *tlp_ctx, uint64_t address)
{
	uint32_t i;

	/* Search through all endpoint devices */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		struct pci_device_config *dev_cfg = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
		if (!dev_cfg->is_endpoint)
			continue;

		/* Check if address falls within BAR0 range (64-bit BAR) */
		uint64_t bar0_base = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
				     (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
		uint64_t bar0_size = BAR_SIZE_16K;

		if (bar0_base != 0 && address >= bar0_base && address < (bar0_base + bar0_size)) {
			return dev_cfg;
		}
	}

	/* Return dummy device for invalid addresses */
	return &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
}

/*
 * Return Unsupported Request (UR) completion for invalid requests
 *
 * @tlp_req [in]: TLP request
 * @tlp_ctx [in]: TLP context
 * @req_header [in]: TLP request header
 */
static inline void return_unsupported_request(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct tlp_context *tlp_ctx,
					      const void *req_header)
{
	void *cpl_header = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);
	struct pci_device_config *dummy_dev = &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
	uint16_t req_id = GET_TLP_REQ_REQ_ID(req_header);
	uint8_t tag9 = GET_TLP_REQ_TAG9(req_header);
	uint8_t tag8 = GET_TLP_REQ_TAG8(req_header);
	uint8_t tag = GET_TLP_REQ_TAG(req_header);
	SET_CMPL_CONTEXT(dummy_dev, req_id, tag9, tag8, tag, TLP_FMT_CPL_NODATA, TLP_CPL_STATUS_UR, 0);
	set_tlp_req_completion_header(cpl_header, dummy_dev, BYTES_IN_DWORD);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
}

/*
 * Find target device for TLP request (memory or config)
 *
 * @tlp_req [in]: TLP request
 * @tlp_ctx [in]: TLP context
 * @tlp_type [in]: TLP request type
 * @return: Pointer to device configuration (never returns NULL, returns dummy device for invalid requests)
 */
static inline struct pci_device_config *find_target_device(struct doca_devemu_pci_tlp_channel_req *tlp_req,
							   struct tlp_context *tlp_ctx,
							   enum tlp_req_type tlp_type)
{
	struct pci_device_config *dev_cfg = NULL;
	const void *req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);

	if (tlp_type == TLP_REQ_TYPE_MEMORY_READ || tlp_type == TLP_REQ_TYPE_MEMORY_WRITE) {
		uint8_t req_fmt = GET_TLP_REQ_FMT(req_header);
		uint64_t address;
		if (req_fmt == TLP_FMT_4DW_NODATA || req_fmt == TLP_FMT_4DW_W_DATA) {
			uint32_t addr_high = GET_MEM_ADDR_64BIT_HIGH(req_header);
			uint32_t addr_low = GET_MEM_ADDR_64BIT_LOW(req_header);
			address = ((uint64_t)addr_high << 32) | (addr_low << 2);
		} else {
			address = GET_MEM_ADDR_32BIT(req_header);
		}
		dev_cfg = find_device_by_address(tlp_ctx, address);
	} else {
		uint8_t bus = GET_TLP_REQ_BUS(req_header);
		uint8_t device = GET_TLP_REQ_DEVICE(req_header);
		uint8_t function = GET_TLP_REQ_FUNCTION(req_header);
		uint8_t req_type = GET_TLP_REQ_TYPE(req_header);
		uint16_t tlp_bdf = BDF(bus, device, function);
		/* Route request to the correct sub-topology based on which NV switch TLP DSP it arrived on */
		uint8_t dsp_id = doca_devemu_pci_tlp_channel_req_get_dsp_id(tlp_req);
		dev_cfg = find_device_by_bdf(tlp_ctx, dsp_id, bus, device, function, req_type);
		/* Set BDF and completer_id on first access */
		if (!dev_cfg->is_bdf_set) {
			dev_cfg->bdf = tlp_bdf;
			dev_cfg->bus = bus;
			dev_cfg->completer_id = tlp_bdf;
			dev_cfg->is_bdf_set = true;
		}
	}
	return dev_cfg;
}

/*
 * Handle TLP request for bridge topology (unified entry point)
 * Follows tlp_handler sample style with clear structure
 *
 * @tlp_req [in]: TLP request
 * @dev_cfg [in]: Target device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_type [in]: TLP request type
 */
static inline void handle_tlp_req(struct doca_devemu_pci_tlp_channel_req *tlp_req,
				  struct pci_device_config *dev_cfg,
				  struct tlp_context *tlp_ctx,
				  enum tlp_req_type tlp_type)
{
	const void *req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t ext_reg_num = GET_TLP_REQ_EXT_REG_NUM(req_header);
	uint16_t req_id = GET_TLP_REQ_REQ_ID(req_header);
	uint8_t tag9 = GET_TLP_REQ_TAG9(req_header);
	uint8_t tag8 = GET_TLP_REQ_TAG8(req_header);
	uint8_t tag = GET_TLP_REQ_TAG(req_header);
	uint16_t cap_id = 0;
	bool is_cap_id_valid = false;
	bool is_pcie_cap = false;

	/* Store requester info in device context */
	dev_cfg->requester_id = req_id;
	dev_cfg->tag9 = tag9;
	dev_cfg->tag8 = tag8;
	dev_cfg->tag = tag;

	/* Handle Dummy device - return UR */
	if (dev_cfg->is_dummy) {
		return_unsupported_request(tlp_req, tlp_ctx, req_header);
		return;
	}

	switch (tlp_type) {
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_0: {
		handle_tlp_req_read_type_0(tlp_req,
					   dev_cfg,
					   tlp_ctx,
					   ext_reg_num,
					   &cap_id,
					   &is_cap_id_valid,
					   &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_1: {
		handle_tlp_req_read_type_1(tlp_req,
					   dev_cfg,
					   tlp_ctx,
					   ext_reg_num,
					   &cap_id,
					   &is_cap_id_valid,
					   &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0: {
		handle_tlp_req_write_type_0(tlp_req,
					    dev_cfg,
					    tlp_ctx,
					    ext_reg_num,
					    req_header,
					    &cap_id,
					    &is_cap_id_valid,
					    &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1: {
		handle_tlp_req_write_type_1(tlp_req,
					    dev_cfg,
					    tlp_ctx,
					    ext_reg_num,
					    req_header,
					    &cap_id,
					    &is_cap_id_valid,
					    &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_MEMORY_READ: {
		handle_tlp_req_memory_read(tlp_req, dev_cfg, tlp_ctx, req_header);
		break;
	}
	case TLP_REQ_TYPE_MEMORY_WRITE: {
		handle_tlp_req_memory_write(tlp_req, dev_cfg, tlp_ctx, req_header);
		break;
	}
	default:
		DOCA_LOG_ERR("Unsupported TLP request type: %d", tlp_type);
		return_unsupported_request(tlp_req, tlp_ctx, req_header);
		return;
	}

	/* Complete memory requests (non-config) */
	uint8_t is_non_posted = (uint8_t)(tlp_type == TLP_REQ_TYPE_MEMORY_READ);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, is_non_posted, dev_cfg->tlp_dev);
}

/**
 * Handle PCI_EVENT request
 *
 * @tlp_req [in]: Pointer to TLP request
 */
static void handle_tlp_req_pci_event(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	enum doca_devemu_pci_tlp_channel_req_pci_event_opmode pci_event_opmode =
		doca_devemu_pci_tlp_channel_req_get_pci_event_opmode(tlp_req);
	switch (pci_event_opmode) {
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_ASSERT:
		DOCA_LOG_DBG("PERST# is asserted (enters reset)");
		break;
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_DEASSERT:
		DOCA_LOG_DBG("PERST# is deasserted (released from reset)");
		break;
	default:
		DOCA_LOG_ERR("Unknown PCI_EVENT opmode: %d", pci_event_opmode);
		break;
	}

	/* Complete the PCI_EVENT request */
	doca_devemu_pci_tlp_channel_req_complete_pci_event(tlp_req);
}

/*
 * TLP request callback - handles incoming TLP requests
 *
 * @channel [in]: TLP channel
 * @tlp_req [in]: TLP request
 * @req_user_data [in]: User-provided request data (unused)
 */
static void tlp_req_handler_cb(struct doca_devemu_pci_tlp_channel *channel,
			       struct doca_devemu_pci_tlp_channel_req *tlp_req,
			       void *req_user_data)
{
	(void)req_user_data;

	struct tlp_context *tlp_ctx;
	union doca_data channel_user_data;
	doca_error_t result;

	/* Get TLP context from channel user data */
	result = doca_ctx_get_user_data(doca_devemu_pci_tlp_channel_as_ctx(channel), &channel_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from tlp channel context: %s", doca_error_get_descr(result));
		return;
	}
	tlp_ctx = (struct tlp_context *)channel_user_data.ptr;

	/* Check for ACG (Asynchronous Credit Grant) opcode */
	enum doca_devemu_pci_tlp_channel_req_opcode opcode = doca_devemu_pci_tlp_channel_req_get_opcode(tlp_req);
	if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_ACG) {
		DOCA_LOG_DBG("Received ACG credit from FW");
		doca_error_t ret = acg_queue_push(tlp_ctx, tlp_req);
		if (ret == DOCA_ERROR_NO_MEMORY) {
			if (tlp_ctx->acg_queue == NULL) {
				DOCA_LOG_DBG("ACG queue unavailable (cleanup in progress), discarding credit");
			} else {
				DOCA_LOG_DBG("ACG queue full (%u/%u), discarding new credit",
					     tlp_ctx->acg_queue_count,
					     tlp_ctx->acg_queue_size);
			}
			doca_devemu_pci_tlp_channel_req_complete_acg(
				tlp_req,
				0,
				DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		} else {
			DOCA_LOG_DBG("ACG credit queued (%u/%u)", tlp_ctx->acg_queue_count, tlp_ctx->acg_queue_size);
		}
		return;
	}

	if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_PCI_EVENT) {
		handle_tlp_req_pci_event(tlp_req);
		return;
	}

	/* Get TLP request type */
	enum tlp_req_type tlp_type = get_tlp_req_type(tlp_req);

	/* Find target device based on TLP type (memory vs config) */
	struct pci_device_config *dev_cfg = find_target_device(tlp_req, tlp_ctx, tlp_type);

	/* Reset the completion fields, before handle request */
	dev_cfg->cmpl_fmt = 0;
	dev_cfg->cmpl_length = 0;
	dev_cfg->cmpl_type = 0;
	dev_cfg->cmpl_status = 0;
	/* Handle TLP request for the found device */
	handle_tlp_req(tlp_req, dev_cfg, tlp_ctx, tlp_type);
}

/*
 * Configure BAR layout for PCI type
 *
 * @pci_type [in]: PCI type object
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t configure_bar_layout_for_custom_pci_type(struct doca_devemu_pci_type *pci_type,
							     const struct pci_type_conf *type_conf)
{
	doca_error_t result;
	const struct bar_memory_layout_config *layout_config;
	const struct bar_db_region_config *db_config;
	const struct bar_region_config *region_config;
	uint64_t region_shift_offset;
	int idx;

	/*
	 * Configure BAR memory layout - Required for endpoints
	 * This tells DOCA framework the size and type of BARs needed
	 * The actual BAR addresses will be assigned by the host during enumeration
	 */
	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MEMORY_LAYOUT; ++idx) {
		layout_config = &layout_configs[idx];
		uint8_t log_bar_size = layout_config->log_size;
		/* Need to extend the log size to contain all bar regions, if we shift all bar regions 4KB right */
		if (type_conf->shift_all_bar_regions_4KB_right && log_bar_size > 0) {
			log_bar_size += 1;
		}
		result = doca_devemu_pci_type_set_memory_bar_conf(pci_type,
								  layout_config->bar_id,
								  log_bar_size,
								  layout_config->memory_type,
								  layout_config->prefetchable);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set BAR memory layout at index %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_INFO("Configured BAR%d: size=2^%d, type=%d, prefetchable=%d",
			      layout_config->bar_id,
			      log_bar_size,
			      layout_config->memory_type,
			      layout_config->prefetchable);
	}

	if (type_conf->shift_all_bar_regions_4KB_right) {
		region_shift_offset = 0x1000;
	} else {
		region_shift_offset = 0;
	}

	/* Configure DB (doorbell) regions */
	for (idx = 0; idx < PCI_TYPE_NUM_BAR_DB_REGIONS; ++idx) {
		db_config = &db_configs[idx];
		if (db_config->with_data)
			result = doca_devemu_pci_type_set_bar_db_region_by_data_conf(pci_type,
										     db_config->region.bar_id,
										     db_config->region.start_address +
											     region_shift_offset,
										     db_config->region.size,
										     db_config->log_db_size,
										     db_config->db_id_msbyte,
										     db_config->db_id_lsbyte);
		else
			result = doca_devemu_pci_type_set_bar_db_region_by_offset_conf(pci_type,
										       db_config->region.bar_id,
										       db_config->region.start_address +
											       region_shift_offset,
										       db_config->region.size,
										       db_config->log_db_size,
										       db_config->log_db_stride_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set DB region at index %d: %s", idx, doca_error_get_descr(result));
			return result;
		}
	}

	/* Configure transaction regions */
	for (idx = 0; idx < PCI_TYPE_NUM_BAR_TRANSACTION_REGIONS; ++idx) {
		region_config = &transaction_configs[idx];
		result = doca_devemu_pci_tlp_type_set_bar_transaction_region_conf(pci_type,
										  region_config->bar_id,
										  region_config->start_address +
											  region_shift_offset,
										  region_config->size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set transaction region at index %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	/* Configure MSI-X table regions */
	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_TABLE_REGIONS; ++idx) {
		region_config = &msix_table_configs[idx];
		result = doca_devemu_pci_type_set_bar_msix_table_region_conf(pci_type,
									     region_config->bar_id,
									     region_config->start_address +
										     region_shift_offset,
									     region_config->size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X table region at index %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	/* Configure MSI-X PBA (Pending Bit Array) regions */
	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_PBA_REGIONS; ++idx) {
		region_config = &msix_pba_configs[idx];
		result = doca_devemu_pci_type_set_bar_msix_pba_region_conf(pci_type,
									   region_config->bar_id,
									   region_config->start_address +
										   region_shift_offset,
									   region_config->size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X pending bit array region at index %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Configure and start PCI TLP type with all required configurations
 * This function mirrors the implementation from devemu_pci_device_tlp_handler sample
 *
 * @pci_type [in]: PCI type object
 * @dev [in]: DOCA device
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t configure_and_start_pci_tlp_type(struct doca_devemu_pci_type *pci_type, struct doca_dev *dev)
{
	doca_error_t result;
	int idx;

	/* Set device for PCI type */
	result = doca_devemu_pci_type_set_dev(pci_type, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set device for PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	/* Configure PCI capabilities */
	for (idx = 0; idx < TLP_PCI_CAPS_NUM; idx++) {
		result = doca_devemu_pci_tlp_type_set_pci_cap_conf(pci_type,
								   tlp_pci_cap_confs[idx].id,
								   tlp_pci_cap_confs[idx].offset,
								   tlp_pci_cap_confs[idx].length);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set PCI capability configuration at idx %d: %s",
				     idx,
				     doca_error_get_descr(result));
			return result;
		}
	}

	/* Configure MSI-X vectors count */
	result = doca_devemu_pci_type_set_num_msix(pci_type, PCI_TYPE_NUM_MSIX);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set number of MSI-X for PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	/* Set the number of doorbells supported by this device type */
	result = doca_devemu_pci_type_set_num_db(pci_type, PCI_TYPE_NUM_DB);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set number of doorbells for PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	/* Start PCI type */
	result = doca_devemu_pci_type_start(pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start PCI type: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("PCI TLP type configured and started successfully");
	return DOCA_SUCCESS;
}

/*
 * Destroy all PCI TLP device types in the TLP context.
 * Stops each started type and releases resources. Entries in pci_type[] are set to NULL.
 *
 * @tlp_ctx [in]: TLP context whose device types are to be destroyed
 */
static void destroy_dev_types(struct tlp_context *tlp_ctx)
{
	doca_error_t result;
	for (uint32_t i = 0; i < tlp_ctx->num_dev_types; i++) {
		if (tlp_ctx->pci_type[i] == NULL)
			continue;

		uint8_t started;
		result = doca_devemu_pci_type_is_started(tlp_ctx->pci_type[i], &started);
		if (result == DOCA_SUCCESS && started) {
			result = doca_devemu_pci_type_stop(tlp_ctx->pci_type[i]);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to stop PCI type: %s", doca_error_get_descr(result));
		}

		result = doca_devemu_pci_type_destroy(tlp_ctx->pci_type[i]);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy PCI type: %s", doca_error_get_descr(result));
		tlp_ctx->pci_type[i] = NULL;
	}
}

/*
 * Create and configure all PCI TLP device types for the TLP context.
 * Each type is created with a unique name, configured with required capabilities, and started.
 * On failure, already-created types are destroyed before returning.
 *
 * @tlp_ctx [in/out]: TLP context to populate with device types
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_dev_types(struct tlp_context *tlp_ctx)
{
	doca_error_t result;
	for (uint32_t i = 0; i < tlp_ctx->num_dev_types; i++) {
		const struct pci_type_conf *type_conf = &pci_type_confs[i];
		result = type_conf->type_create_func(type_conf->name, &(tlp_ctx->pci_type[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create PCI TLP type: %s", doca_error_get_descr(result));
			goto cleanup;
		}

		if (type_conf->is_custom_bar_layout) {
			result = configure_bar_layout_for_custom_pci_type(tlp_ctx->pci_type[i], type_conf);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Unable to configure BAR layout for PCI type: %s",
					     doca_error_get_descr(result));
				goto cleanup;
			}
		}

		result = configure_and_start_pci_tlp_type(tlp_ctx->pci_type[i], tlp_ctx->dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to configure and start PCI TLP type: %s", doca_error_get_descr(result));
			goto cleanup;
		}
	}
	return DOCA_SUCCESS;
cleanup:
	destroy_dev_types(tlp_ctx);
	return result;
}

/*
 * Find device that supports TLP emulation
 *
 * @pci_address [in]: PCI address string to search for
 * @dev [out]: Pointer to store the found device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t find_supported_tlp_device(const char *pci_address,
					      struct doca_dev **dev,
					      uint16_t *max_tlp_types_cap)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t res;
	uint8_t is_equal;
	size_t i;

	*dev = NULL;
	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	for (i = 0; i < nb_devs; i++) {
		if (doca_devinfo_is_equal_pci_addr(dev_list[i], pci_address, &is_equal) != DOCA_SUCCESS ||
		    is_equal == 0)
			continue;

		res = doca_devemu_pci_tlp_cap_get_max_types(dev_list[i], max_tlp_types_cap);
		if (res != DOCA_SUCCESS)
			continue;

		if (*max_tlp_types_cap == 0) {
			DOCA_LOG_WARN("Found device with matching address, but does not support PCI TLP emulation");
			continue;
		}

		res = doca_dev_open(dev_list[i], dev);
		if (res == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			return res;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

/*
 * Initialize TLP channel and register TLP request handler callback
 * This function mirrors the implementation from devemu_pci_device_tlp_handler sample
 *
 * @tlp_ctx [in/out]: TLP context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t init_tlp_channel(struct tlp_context *tlp_ctx)
{
	doca_error_t result;
	union doca_data user_data = {0};

	/* Create progress engine */
	result = doca_pe_create(&tlp_ctx->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create TLP channel */
	result = doca_devemu_pci_tlp_channel_create(tlp_ctx->dev, &tlp_ctx->tlp_channel);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create TLP channel: %s", doca_error_get_descr(result));
		(void)doca_pe_destroy(tlp_ctx->pe);
		tlp_ctx->pe = NULL;
		return result;
	}

	/* Set TLP request user data size */
	result = doca_devemu_pci_tlp_channel_set_req_user_data_size(tlp_ctx->tlp_channel,
								    TLP_CHANNEL_CTX_USER_DATA_SIZE_DEFAULT);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set TLP request user data size: %s", doca_error_get_descr(result));
		(void)doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		tlp_ctx->tlp_channel = NULL;
		(void)doca_pe_destroy(tlp_ctx->pe);
		tlp_ctx->pe = NULL;
		return result;
	}

	/* Register TLP request handler callback */
	result = doca_devemu_pci_tlp_channel_event_req_register(tlp_ctx->tlp_channel, tlp_req_handler_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register TLP request handler: %s", doca_error_get_descr(result));
		(void)doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		tlp_ctx->tlp_channel = NULL;
		(void)doca_pe_destroy(tlp_ctx->pe);
		tlp_ctx->pe = NULL;
		return result;
	}

	/* Enable ACG mechanism to receive credits for MMIO Write (MSI) - required for hotplug mode */
	if (tlp_ctx->hotplug_mode) {
		result = doca_devemu_pci_tlp_channel_set_acg_enabled(tlp_ctx->tlp_channel, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set ACG enabled: %s. ACG is required for hotplug mode",
				     doca_error_get_descr(result));
			(void)doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
			tlp_ctx->tlp_channel = NULL;
			(void)doca_pe_destroy(tlp_ctx->pe);
			tlp_ctx->pe = NULL;
			return result;
		}
		DOCA_LOG_INFO("ACG enabled for MSI interrupts");
	}

	/* Get context from TLP channel */
	tlp_ctx->channel_ctx = doca_devemu_pci_tlp_channel_as_ctx(tlp_ctx->tlp_channel);
	if (tlp_ctx->channel_ctx == NULL) {
		DOCA_LOG_ERR("Failed to get TLP channel context");
		(void)doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		tlp_ctx->tlp_channel = NULL;
		(void)doca_pe_destroy(tlp_ctx->pe);
		tlp_ctx->pe = NULL;
		return DOCA_ERROR_UNEXPECTED;
	}

	/* Connect PE to TLP channel context */
	result = doca_pe_connect_ctx(tlp_ctx->pe, tlp_ctx->channel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect PE to TLP channel context: %s", doca_error_get_descr(result));
		(void)doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		tlp_ctx->tlp_channel = NULL;
		tlp_ctx->channel_ctx = NULL;
		(void)doca_pe_destroy(tlp_ctx->pe);
		tlp_ctx->pe = NULL;
		return result;
	}

	/* Set user data for TLP channel context */
	user_data.ptr = tlp_ctx;
	result = doca_ctx_set_user_data(tlp_ctx->channel_ctx, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set user data for TLP channel context: %s", doca_error_get_descr(result));
		(void)doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		tlp_ctx->tlp_channel = NULL;
		tlp_ctx->channel_ctx = NULL;
		(void)doca_pe_destroy(tlp_ctx->pe);
		tlp_ctx->pe = NULL;
		return result;
	}

	/* Start TLP channel context */
	result = doca_ctx_start(tlp_ctx->channel_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start TLP channel context: %s", doca_error_get_descr(result));
		(void)doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		tlp_ctx->tlp_channel = NULL;
		tlp_ctx->channel_ctx = NULL;
		(void)doca_pe_destroy(tlp_ctx->pe);
		tlp_ctx->pe = NULL;
		return result;
	}

	DOCA_LOG_INFO("TLP channel initialized and started successfully");
	return DOCA_SUCCESS;
}

/*
 * Initialize ACG queue for caching multiple ACG credits
 *
 * @tlp_ctx [in/out]: TLP context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t init_acg_queue(struct tlp_context *tlp_ctx)
{
	doca_error_t result;
	const struct doca_devinfo *devinfo = doca_dev_as_devinfo(tlp_ctx->dev);

	/* Query max ACG credits capability */
	result = doca_devemu_pci_tlp_cap_get_max_acg(devinfo, &tlp_ctx->acg_queue_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query max ACG: %s", doca_error_get_descr(result));
		return result;
	}
	DOCA_LOG_DBG("Max ACG credits: %u", tlp_ctx->acg_queue_size);

	/* Allocate circular queue for ACG credits */
	tlp_ctx->acg_queue =
		(struct doca_devemu_pci_tlp_channel_req **)calloc(tlp_ctx->acg_queue_size,
								  sizeof(struct doca_devemu_pci_tlp_channel_req *));
	if (tlp_ctx->acg_queue == NULL) {
		DOCA_LOG_ERR("Failed to allocate ACG queue");
		return DOCA_ERROR_NO_MEMORY;
	}
	tlp_ctx->acg_queue_head = 0;
	tlp_ctx->acg_queue_tail = 0;
	tlp_ctx->acg_queue_count = 0;
	DOCA_LOG_INFO("ACG queue initialized: capacity=%u", tlp_ctx->acg_queue_size);

	return DOCA_SUCCESS;
}

/*
 * Create all endpoint devices in static mode
 *
 * @tlp_ctx [in/out]: TLP context
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t create_all_devices(struct tlp_context *tlp_ctx)
{
	doca_error_t result;

	if (tlp_ctx->num_ep == 0) {
		DOCA_LOG_INFO("No devices to create (bridge-only topology)");
		return DOCA_SUCCESS;
	}

	DOCA_LOG_INFO("Creating %u endpoint device(s)", tlp_ctx->num_ep);

	for (uint32_t i = 0; i < tlp_ctx->num_ep; i++) {
		struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
		struct pci_device_config *dsp = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i];

		result = create_device(tlp_ctx, ep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create EP %u: %s", i, doca_error_get_descr(result));
			return result;
		}

		/* Static mode: Set Power ON (bit10=0) and DLActive (device already present at boot) */
		dsp->caps.express.slot_control &= ~SLOT_CTRL_POWER_CONTROLLER;
		dsp->caps.express.slot_status |= SLOT_STS_PRESENCE_DETECT_STATE;
		dsp->caps.express.link_status |= LINK_STS_DL_ACTIVE;
		DOCA_LOG_DBG("Static Mode: DSP[%u] initialized with Power ON (bit10=0), PDS=1, DLActive=1", i);
	}

	DOCA_LOG_INFO("All %u endpoint devices created successfully", tlp_ctx->num_ep);
	return DOCA_SUCCESS;
}

/*
 * Cleanup resources
 *
 * @tlp_ctx [in/out]: TLP context to clean up
 */
static void devemu_pci_tlp_bridge_handler_cleanup(struct tlp_context *tlp_ctx)
{
	uint32_t i;

	if (tlp_ctx == NULL)
		return;

	/* Flush all pending ACG requests from queue */
	if (tlp_ctx->acg_queue != NULL) {
		struct doca_devemu_pci_tlp_channel_req *acg_req;
		uint16_t count = 0;
		while ((acg_req = acg_queue_pop(tlp_ctx)) != NULL) {
			doca_devemu_pci_tlp_channel_req_complete_acg(
				acg_req,
				0,
				DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
			count++;
		}
		DOCA_LOG_DBG("Flushed %u ACG credits", count);
		free(tlp_ctx->acg_queue);
		tlp_ctx->acg_queue = NULL;
	}

	/* Clean up all devices (both hotplug and static) */
	if (tlp_ctx->devs_config != NULL) {
		for (i = 0; i < tlp_ctx->num_ep; i++) {
			struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
			if (ep->device_present || ep->rep != NULL || ep->tlp_dev != NULL) {
				DOCA_LOG_INFO("Cleaning up EP %u", i);
				(void)destroy_device(ep);
			}
		}
	}

	bool channel_ctx_in_progress = false;
	if (tlp_ctx->channel_ctx != NULL) {
		doca_error_t result = doca_ctx_stop(tlp_ctx->channel_ctx);
		if (result == DOCA_ERROR_IN_PROGRESS)
			channel_ctx_in_progress = true;
		else if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop TLP channel context: %s", doca_error_get_descr(result));
		else
			tlp_ctx->channel_ctx = NULL;
	}

	/* Drain the TLP channel if stop is in progress */
	while (channel_ctx_in_progress) {
		if (tlp_ctx->pe != NULL)
			(void)doca_pe_progress(tlp_ctx->pe);

		enum doca_ctx_states ctx_state;
		doca_error_t result = doca_ctx_get_state(tlp_ctx->channel_ctx, &ctx_state);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to get state of channel context: %s", doca_error_get_descr(result));
		else if (ctx_state == DOCA_CTX_STATE_IDLE) {
			tlp_ctx->channel_ctx = NULL;
			channel_ctx_in_progress = false;
		}
	}

	if (tlp_ctx->tlp_channel != NULL) {
		doca_error_t result = doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy TLP channel: %s", doca_error_get_descr(result));
		tlp_ctx->tlp_channel = NULL;
	}

	if (tlp_ctx->pe != NULL) {
		doca_error_t result = doca_pe_destroy(tlp_ctx->pe);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy PE: %s", doca_error_get_descr(result));
		tlp_ctx->pe = NULL;
	}

	destroy_dev_types(tlp_ctx);

	if (tlp_ctx->dev != NULL) {
		doca_error_t result = doca_dev_close(tlp_ctx->dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(result));
		tlp_ctx->dev = NULL;
	}

	if (tlp_ctx->devs_config != NULL) {
		free(tlp_ctx->devs_config);
		tlp_ctx->devs_config = NULL;
	}

	/* Free BDF map entries */
	if (tlp_ctx->bdf_entries != NULL) {
		free(tlp_ctx->bdf_entries);
		tlp_ctx->bdf_entries = NULL;
		memset(tlp_ctx->bdf_map, 0, sizeof(tlp_ctx->bdf_map));
	}

	/* Free all transaction region memories for each PF */
	if (tlp_ctx->transaction_region_memories != NULL) {
		for (i = 0; i < tlp_ctx->num_ep; i++) {
			if (tlp_ctx->transaction_region_memories[i] != NULL) {
				free(tlp_ctx->transaction_region_memories[i]);
				tlp_ctx->transaction_region_memories[i] = NULL;
			}
		}
		free(tlp_ctx->transaction_region_memories);
		tlp_ctx->transaction_region_memories = NULL;
		tlp_ctx->transaction_region_size = 0;
	}

	DOCA_LOG_INFO("Cleanup completed");
}

/*
 * Initialize TLP context
 *
 * @ctx [in/out]: TLP context to initialize
 * @num_dev_types [in]: Number of device types to be used
 * @num_ep [in]: Total number of endpoints to distribute across NV switch TLP DSPs
 * @num_nv_switch_tlp_dsp [in]: Number of NV switch TLP DSPs on the TLP channel
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_tlp_context(struct tlp_context *ctx,
				     uint32_t num_dev_types,
				     uint32_t num_ep,
				     uint8_t num_nv_switch_tlp_dsp)
{
	/* Sanity check to avoid division by zero */
	if (num_nv_switch_tlp_dsp == 0) {
		DOCA_LOG_ERR("Invalid number of NV switch TLP DSPs: %u", num_nv_switch_tlp_dsp);
		return DOCA_ERROR_INVALID_VALUE;
	}

	ctx->num_ep = num_ep;
	ctx->num_dsp = num_ep; /* There's one DSP per EP */
	ctx->num_nv_switch_tlp_dsp = num_nv_switch_tlp_dsp;
	ctx->num_bridges = num_nv_switch_tlp_dsp + num_ep;
	ctx->num_ep_per_nv_switch_tlp_dsp = num_ep / num_nv_switch_tlp_dsp;
	ctx->num_ep_extra_per_nv_switch_tlp_dsp = num_ep % num_nv_switch_tlp_dsp;
	ctx->num_devices = ctx->num_bridges + num_ep + DUMMY_DEV_NUM;
	ctx->num_dev_types = num_dev_types;

	DOCA_LOG_INFO(
		"Initializing TLP context: %u EP, %u DSP, %u NV switch TLP DSP(s), %u total bridges, %u total devices",
		num_ep,
		num_ep,
		num_nv_switch_tlp_dsp,
		ctx->num_bridges,
		ctx->num_devices);

	/* Allocate device array - reps and tlp_devs will be stored in pci_device_config */
	ctx->devs_config = (struct pci_device_config *)calloc(ctx->num_devices, sizeof(struct pci_device_config));
	if (ctx->devs_config == NULL) {
		DOCA_LOG_ERR("Failed to allocate device array");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Allocate BDF map entries - one per device */
	ctx->bdf_entries = (struct bdf_map_entry *)calloc(ctx->num_devices, sizeof(struct bdf_map_entry));
	if (ctx->bdf_entries == NULL) {
		DOCA_LOG_ERR("Failed to allocate BDF map entries");
		free(ctx->devs_config);
		ctx->devs_config = NULL;
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize transaction region memory for MMIO
 * Allocates independent memory region for each PF
 *
 * @tlp_ctx [in/out]: TLP context
 * @size [in]: Size of each transaction region
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t init_transaction_region(struct tlp_context *tlp_ctx, size_t size)
{
	uint32_t i;

	if (tlp_ctx->num_ep == 0) {
		DOCA_LOG_INFO("No transaction regions to create (bridge-only topology)");
		return DOCA_SUCCESS;
	}

	/* Allocate array of pointers for transaction region memories */
	tlp_ctx->transaction_region_memories = (void **)calloc(tlp_ctx->num_ep, sizeof(void *));
	if (!tlp_ctx->transaction_region_memories) {
		DOCA_LOG_ERR("Failed to allocate transaction region memory array");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Allocate independent memory region for each PF */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		tlp_ctx->transaction_region_memories[i] = malloc(size);
		if (!tlp_ctx->transaction_region_memories[i]) {
			DOCA_LOG_ERR("Failed to allocate transaction region memory for PF %u", i);
			/* Cleanup already allocated regions */
			while (i-- > 0) {
				free(tlp_ctx->transaction_region_memories[i]);
				tlp_ctx->transaction_region_memories[i] = NULL;
			}
			free(tlp_ctx->transaction_region_memories);
			tlp_ctx->transaction_region_memories = NULL;
			return DOCA_ERROR_NO_MEMORY;
		}
		memset(tlp_ctx->transaction_region_memories[i], 0xAA, size);
		DOCA_LOG_INFO("Transaction region initialized for PF %u: size=%zu bytes, base_addr=%p",
			      i,
			      size,
			      tlp_ctx->transaction_region_memories[i]);
	}

	tlp_ctx->transaction_region_size = size;
	DOCA_LOG_INFO("All transaction regions initialized: %u regions, %zu bytes each", tlp_ctx->num_ep, size);
	return DOCA_SUCCESS;
}

/**
 * Sample's logic for handling PCI TLP bridge requests
 *
 * @pci_address [in]: PCI address string for the device
 * @num_dev_types [in]: Number of device types to create
 * @num_ep [in]: Number of endpoints to create (1-32)
 * @hotplug_mode [in]: Hotplug mode: true (1) for hotplug mode, false (0) for static mode
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_tlp_bridge_handler_dpu(const char *pci_address,
					       uint32_t num_dev_types,
					       uint32_t num_ep,
					       bool hotplug_mode)
{
	struct tlp_context tlp_ctx = {0};
	doca_error_t result;

	DOCA_LOG_INFO("Starting TLP Bridge Handler");

	/* Set up signal handlers for graceful shutdown */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	tlp_ctx.hotplug_mode = hotplug_mode;

	/* Open device — required by both the channel and the PCI TLP type */
	uint16_t max_tlp_types_cap = 0;
	result = find_supported_tlp_device(pci_address, &tlp_ctx.dev, &max_tlp_types_cap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to find supported device: %s", doca_error_get_descr(result));
		return result;
	}

	/* Initialize TLP channel and register request handler */
	result = init_tlp_channel(&tlp_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize TLP channel: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/*
	 * Query num_dsp now that the channel is started, then initialize all context
	 * fields and sub-arrays sized according to the actual DSP count.
	 */
	uint8_t num_nv_switch_tlp_dsp;
	result = doca_devemu_pci_tlp_channel_get_num_dsp(tlp_ctx.tlp_channel, &num_nv_switch_tlp_dsp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query number of DSPs: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	DOCA_LOG_INFO("TLP Bridge Handler will use: %u device types, %u USP + %u DSPs + %u EPs (%s mode)",
		      num_dev_types,
		      num_nv_switch_tlp_dsp,
		      num_ep,
		      num_ep,
		      hotplug_mode ? "Hotplug" : "Static");

	result = init_tlp_context(&tlp_ctx, num_dev_types, num_ep, num_nv_switch_tlp_dsp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize TLP context: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	if (tlp_ctx.num_dev_types > max_tlp_types_cap) {
		DOCA_LOG_INFO("Requested dev types num (%u) is greater than cap (%u), reducing to %u",
			      tlp_ctx.num_dev_types,
			      max_tlp_types_cap,
			      max_tlp_types_cap);
		tlp_ctx.num_dev_types = max_tlp_types_cap;
	}

	if (tlp_ctx.num_dev_types < MIN_TLP_PCI_TYPE_NUM) {
		DOCA_LOG_ERR("Device capability only supports %u types, but minimum is %u",
			     max_tlp_types_cap,
			     MIN_TLP_PCI_TYPE_NUM);
		result = DOCA_ERROR_INVALID_VALUE;
		goto cleanup;
	}

	/* Create PCI TLP types */
	result = create_dev_types(&tlp_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create PCI TLP types: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/* Initialize ACG queue for MSI interrupt credits */
	result = init_acg_queue(&tlp_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize ACG queue: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/* Initialize device topology (software configuration) */
	init_device_topology(&tlp_ctx);

	/* Create all devices in static mode */
	if (!hotplug_mode) {
		DOCA_LOG_INFO("Static Mode: Creating all %u EPs at startup", num_ep);
		result = create_all_devices(&tlp_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create devices: %s", doca_error_get_descr(result));
			goto cleanup;
		}
	} else {
		DOCA_LOG_INFO("Hotplug Mode: EPs will be created dynamically");
	}

	/* Initialize transaction region for MMIO */
	result = init_transaction_region(&tlp_ctx, TRANSACTION_REGION_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize transaction region: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	DOCA_LOG_INFO("TLP channel started, waiting for requests...");
	DOCA_LOG_INFO("Topology: %u bridges (%u USP + %u DSPs), %u endpoints (%u Single-PF)",
		      tlp_ctx.num_bridges,
		      tlp_ctx.num_nv_switch_tlp_dsp,
		      tlp_ctx.num_dsp,
		      tlp_ctx.num_ep,
		      tlp_ctx.num_ep);
	DOCA_LOG_INFO("Press Ctrl+C to exit");

	if (hotplug_mode) {
		DOCA_LOG_INFO("Hotplug control (enter commands):");
		DOCA_LOG_INFO("  plug <DSP_IDX>   - Plug device to DSP slot");
		DOCA_LOG_INFO("  unplug <DSP_IDX> - Unplug device from DSP slot");
		DOCA_LOG_INFO("Example: plug 0");
	}

	/* Poll on the TLP channel to get TLP requests */
	while (!force_quit) {
		doca_pe_progress(tlp_ctx.pe);

		/* Check for user input in hotplug mode (non-blocking) */
		if (hotplug_mode && stdin_has_input()) {
			uint32_t dsp_idx;
			bool is_plug;
			doca_error_t ret = read_hotplug_command(tlp_ctx.num_dsp, &dsp_idx, &is_plug);
			if (ret == DOCA_SUCCESS) {
				DOCA_LOG_INFO("%s on DSP[%u]", is_plug ? "PLUG" : "UNPLUG", dsp_idx);
				ret = trigger_hotplug_event(&tlp_ctx, dsp_idx, is_plug);
				if (ret == DOCA_SUCCESS) {
					DOCA_LOG_INFO("Operation completed successfully");
				} else if (ret == DOCA_ERROR_AGAIN) {
					DOCA_LOG_INFO("No ACG credit available, please try again");
				} else {
					DOCA_LOG_ERR("Operation failed: %s", doca_error_get_descr(ret));
				}
			}
		}
	}
	DOCA_LOG_INFO("Exiting...");
	DOCA_LOG_INFO("Sample completed successfully");

cleanup:
	devemu_pci_tlp_bridge_handler_cleanup(&tlp_ctx);
	return result;
}
