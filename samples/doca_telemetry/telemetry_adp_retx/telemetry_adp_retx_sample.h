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

#include <stdlib.h>

#include <doca_error.h>
#include <doca_telemetry_adp_retx.h>

#define NO_VHCA_ID 0xFFFFFFFF

/* Configuration struct */
struct telemetry_adp_retx_sample_cfg {
	uint8_t pci_set;						 /**< Whether the user provided a pci address */
	char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];			 /**< PCI address to be used */
	enum doca_telemetry_adp_retx_hist_time_unit time_unit;		 /**< Time unit to use is histograms */
	enum doca_telemetry_adp_retx_hist_bin_width_mode bin_width_mode; /**< Bin width mode use is histograms */
	uint32_t bin_num;						 /**< Number of histogram bins */
	uint32_t vhca_id;						 /**< VHCA ID to use */
	uint16_t bin0_width;						 /**< Width for bin 0 */
	uint16_t bin1_width;						 /**< Width of bin 1 to bin_num */
	bool clear_on_read;						 /**< Reset histogram after a read */
	uint32_t wait_time;						 /**< Time to wait before getting stats */
	uint32_t num_reads;						 /**< Number of times to read histogram */
};

/*
 * Run sample
 *
 * @cfg [in]: sample configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
doca_error_t telemetry_adp_retx_sample_run(const struct telemetry_adp_retx_sample_cfg *cfg);
