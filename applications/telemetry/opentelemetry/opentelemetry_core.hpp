/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef OPENTELEMETRY_CORE_HPP_
#define OPENTELEMETRY_CORE_HPP_

#include <limits.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_telemetry_diag.h>

#include "opentelemetry/sdk/metrics/meter_provider.h"

/* Defines to indicate no label present */
#define DATA_ID_NO_PRIORITY 0xFF
#define DATA_ID_NO_PORT 0xFFFF

struct diag_data_id {
	uint64_t data_id;
	std::string name;
	std::string desc;
	std::string dir;
	std::string unit;
	uint16_t port;
	uint8_t priority;
};

struct opentelemetry_cfg {
	char dev_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* Device PCI address to get diag stats from */
	char host_name[HOST_NAME_MAX + 1];	       /* Host name of system in use */
	std::vector<diag_data_id> diag_data;	       /* List of data_ids to use along with metadata */
	std::string export_ip;			       /* IPv4 address to export OpenTelemetry payloads to */
	uint32_t export_time;			       /* Time in milliseconds between OpenTelemetry exports */
	uint8_t verbose;			       /* Run in verbose mode */
};

struct opentelemetry_ctx {
	using opentelemetry_provider = opentelemetry::sdk::metrics::MeterProvider;
	using opentelemetry_counter = opentelemetry::v1::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>;

	doca_dev *dev;						/* Doca device opened from input PCI address */
	doca_telemetry_diag *diag;				/* Doca diag context for telemetry */
	std::shared_ptr<opentelemetry_provider> meter_provider; /* OpenTelemetry metrics provider */
	std::vector<opentelemetry_counter> counters;		/* OpenTelemetry counters */
	std::vector<std::map<std::string, std::string>> labels; /* OpenTelemetry labels for each counter */
};

/*
 * Initialise the runtime data for the app
 *
 * @app_cfg [in]: OpenTelemetry configuration file
 * @opentel_ctx [out]: Initialised app ctx data
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t opentelemetry_init(const struct opentelemetry_cfg *app_cfg,
				struct opentelemetry_ctx *opentel_ctx) noexcept;

/*
 * Close and destroy runtime data initialised in the app
 *
 * @opentel_ctx [in]: App ctx data to destroy
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t opentelemetry_destroy(struct opentelemetry_ctx *opentel_ctx) noexcept;

/*
 * Main program loop
 *
 * @app_cfg [in]: OpenTelemetry configuration file
 * @opentel_ctx [in]: Initialised app context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t opentelemetry_run(const struct opentelemetry_cfg *app_cfg, struct opentelemetry_ctx *opentel_ctx) noexcept;

#endif /* OPENTELEMETRY_CORE_HPP_ */
