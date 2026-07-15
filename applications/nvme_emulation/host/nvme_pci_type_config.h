/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef DEVEMU_PCI_TYPE_CONFIG_H_
#define DEVEMU_PCI_TYPE_CONFIG_H_

#include <doca_devemu_pci_type.h>

#define NVME_TYPE_NAME "NVME Type"

#define PCI_TYPE_DEVICE_ID 0x6001
#define PCI_TYPE_VENDOR_ID 0x15b3
#define PCI_TYPE_SUBSYSTEM_ID 0x0051
#define PCI_TYPE_SUBSYSTEM_VENDOR_ID 0x15b3
#define PCI_TYPE_REVISION_ID 0
#define PCI_TYPE_CLASS_CODE 0x010802

struct bar_memory_layout_config {
	uint8_t bar_id;				       /**< The BAR ID that this layout describes */
	uint16_t log_size;			       /**< The log size of the BAR */
	enum doca_devemu_pci_bar_mem_type memory_type; /**< The memory type of the BAR */
	uint8_t prefetchable;			       /**< Whether the BAR memory is prefetchable */
};

struct bar_region_config {
	uint8_t bar_id;		/**< The BAR ID that the region is part of */
	uint64_t start_address; /**< The start address of the region within the BAR */
	uint64_t size;		/**< The size of the region in bytes */
};

struct bar_db_region_config {
	struct bar_region_config region; /**< Holds common BAR region configuration */
	uint8_t log_db_size;		 /**< The log size of a single doorbell record */
	uint8_t log_db_stride_size;	 /**< The log stride size of a single doorbell record */
	bool with_data;			 /**< If true the doorbell will work by data. Otherwise by offset */
	uint16_t db_id_msbyte;		 /**< Relevant only when with_data is true */
	uint16_t db_id_lsbyte;		 /**< Relevant only when with_data is true */
};

/* Configure how many BARs exist, and what they look like */
#define PCI_TYPE_NUM_BAR_MEMORY_LAYOUT 2

static const struct bar_memory_layout_config layout_configs[PCI_TYPE_NUM_BAR_MEMORY_LAYOUT] = {
	[0] =
		{
			.bar_id = 0,
			.log_size = 0xf,
			.memory_type = DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT,
			.prefetchable = 0,
		},
	[1] =
		{
			.bar_id = 1,
			.log_size = 0x0,
			.memory_type = DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT,
			.prefetchable = 0,
		},
};

/* Configure how many MSI-X vectors there are, and where they are placed in the BAR */
#define PCI_TYPE_NUM_MSIX 4
#define PCI_TYPE_NUM_BAR_MSIX_TABLE_REGIONS 1
#define PCI_TYPE_NUM_BAR_MSIX_PBA_REGIONS 1

static const struct bar_region_config msix_table_configs[PCI_TYPE_NUM_BAR_MSIX_TABLE_REGIONS] = {
	[0] =
		{
			.bar_id = 0,
			.start_address = 0x2000,
			.size = (1 << 12),
		},
};

static const struct bar_region_config msix_pba_configs[PCI_TYPE_NUM_BAR_MSIX_PBA_REGIONS] = {
	[0] =
		{
			.bar_id = 0,
			.start_address = 0x3000,
			.size = (1 << 12),
		},
};

/* Configure number of doorbells and regions and how they operate */
#define PCI_TYPE_NUM_BAR_DB_REGIONS 1

static const struct bar_db_region_config db_configs[PCI_TYPE_NUM_BAR_DB_REGIONS] = {
	[0] =
		{
			.region.bar_id = 0,
			.region.start_address = 0x1000,
			.region.size = (1 << 12),
			.log_db_size = 0x2,
			.log_db_stride_size = 0x2,
			.with_data = false,
		},
};

/* Configure number of stateful regions and their ranges */
#define PCI_TYPE_NUM_BAR_STATEFUL_REGIONS 1
#define PCI_TYPE_MAX_STATEFUL_REGION_SIZE 2048

static const struct bar_region_config stateful_configs[PCI_TYPE_NUM_BAR_STATEFUL_REGIONS] = {
	[0] =
		{
			.bar_id = 0,
			.start_address = 0x0000,
			.size = (1 << 7),
		},
};

#endif // DEVEMU_PCI_TYPE_CONFIG_H_
