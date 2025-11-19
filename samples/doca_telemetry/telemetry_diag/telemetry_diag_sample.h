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
#include <doca_telemetry_diag.h>

#define TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME 256 /* Max file name length including terminating 0*/
#define MAX_NAME_SIZE 256			/* Maximum length for Data ID name string, including terminating 0*/

/* Configuration struct */
struct telemetry_diag_sample_cfg {
	char data_ids_input_path[TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME];		/**< data ids input file path */
	char output_path[TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME];			/**< output file path */
	char data_ids_example_export_path[TELEMETRY_DIAG_SAMPLE_MAX_FILE_NAME]; /**< output file for example json dump
										 */
	uint64_t sample_period;							/**< sample period to be used */
	struct data_id_entry *data_ids_struct;			    /**< array of data_id_entry structures */
	uint32_t run_time;					    /**< total sample run time, in seconds */
	uint32_t max_num_samples_per_read;			    /**< max number of samples for each read */
	uint32_t num_data_ids;					    /**< the number of entries in the data_ids_struct */
	enum doca_telemetry_diag_sync_mode sync_mode;		    /**< sync mode to be used */
	enum doca_telemetry_diag_sample_mode sample_mode;	    /**< sample mode to be used */
	enum doca_telemetry_diag_output_format output_format;	    /**< output format to be used */
	enum doca_telemetry_diag_timestamp_source timestamp_source; /**< timestamp source to be used */
	uint8_t log_max_num_samples;				    /**< log max number of samples to be used */
	uint8_t force_ownership;				    /**< force ownership when creating diag context */
	uint8_t export_json;			   /**< whether the user chose to export example json */
	uint8_t import_json;			   /**< whether an input data_ids json path was given */
	uint8_t pci_set;			   /**< whether the user provided a pci address */
	char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]; /**< PCI address to be used */
	uint64_t reconfig_sample_period;	   /**< reconfig sample period to be used */
};

/*
 * Structure for storing data ID and its associated name.
 *
 */
struct data_id_entry {
	uint64_t data_id;	  /* Data ID */
	char name[MAX_NAME_SIZE]; /* Data ID name */
};

/*
 * Run sample
 *
 * @cfg [in]: sample configuration
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise.
 */
doca_error_t telemetry_diag_sample_run(const struct telemetry_diag_sample_cfg *cfg);
