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

#include <stdlib.h>

#include <doca_error.h>
#include <doca_telemetry_dpa.h>

/* Configuration struct */
struct telemetry_dpa_sample_cfg {
	uint32_t run_time;				   /**< total sample run time, in seconds */
	uint8_t pci_set;				   /**< whether the user provided a pci address */
	char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];	   /**< PCI address to be used */
	uint32_t process_id;				   /**< specific process id */
	uint8_t process_id_set;				   /**< whether the user provided a process id */
	uint32_t thread_id;				   /**< specific threads id */
	uint8_t thread_id_set;				   /**< whether the user provided a thread id */
	enum doca_telemetry_dpa_counter_type counter_type; /**< specific counter type to use */
	uint32_t max_event_sample;			   /**< specific number of event samples to use */
};

/*
 * Run sample
 *
 * @cfg [in]: sample configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
doca_error_t telemetry_dpa_sample_run(const struct telemetry_dpa_sample_cfg *cfg);
