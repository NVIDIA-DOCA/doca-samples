/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>

#include <doca_argp.h>

#include "pcc_core.h"

#include "libflexio/flexio_ver.h"
#define PCC_FLEXIO_MAJOR_VERSION (25)
#define PCC_FLEXIO_MINOR_VERSION (4)
#define PCC_FLEXIO_PATCH_VERSION (0)
#define FLEXIO_VER_USED FLEXIO_VER(PCC_FLEXIO_MAJOR_VERSION, PCC_FLEXIO_MINOR_VERSION, PCC_FLEXIO_PATCH_VERSION)
#include "libflexio/flexio.h"

/*
 * Formats of the trace message to be printed from the device
 */
static char *trace_message_formats[] = {
	"format 0 - user init: port num = %#lx, algo index = %#lx, algo slot = %#lx, algo enable = %#lx, disable event bitmask = %#lx\n",
	"format 1 - user algo: algo slot = %#lx, result rate = %#lx, result rtt req = %#lx, port num = %#lx, timestamp = %#lx\n",
	"format 2 - pcc_np dev: thread_idx = %#lx, debug_init = %#lx, cnt_enter = %#lx, cnt_arm = %#lx. end %u\n",
	"format 3 - pcc_np dev: thread_idx = %#lx, cnt_recv_packet = %#lx, cnt_sent_packet = %#lx, cnt_user_func_err = %#lx. end %u\n",
	"format 4 - pcc_np dev: thread_idx = %#lx, rq = %#lx, rq_pi = %#lx, rqcq = %#lx, rqcq_ci = %#lx\n",
	"format 5 - pcc_np dev: thread_idx = %#lx, sq = %#lx, sq_pi = %#lx, sqcq = %#lx. end %u\n",
	NULL};

/* Default PCC RP threads */
const uint32_t default_pcc_rp_threads_list[PCC_RP_THREADS_NUM_DEFAULT_VALUE] = {
	176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 192, 193, 194, 195, 196,
	197, 198, 199, 200, 201, 202, 203, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217,
	218, 219, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 240};
/* Default PCC NP threads */
const uint32_t default_pcc_np_threads_list[PCC_NP_THREADS_NUM_DEFAULT_VALUE] =
	{16, 17, 18, 19, 20, 21, 22, 23, 32, 33, 34, 35, 36, 37, 38, 39, 48};

/*
 * Declare threads list flag
 */
static bool use_threads_list = false;

/*
 * Declare DPA resources flag
 */
static bool use_dpa_resources = false;

/*
 * Declare user set application flag
 */
static bool user_set_app = false;

/**
 * @brief Get the size of a file
 *
 * @param[in] path - Path to the file
 * @param[out] file_size - Size of the file in bytes
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_file_size(const char *path, size_t *file_size)
{
	FILE *file;
	long nb_file_bytes;

	file = fopen(path, "rb");
	if (file == NULL)
		return DOCA_ERROR_NOT_FOUND;

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return DOCA_ERROR_IO_FAILED;
	}

	nb_file_bytes = ftell(file);
	fclose(file);

	if (nb_file_bytes == -1)
		return DOCA_ERROR_IO_FAILED;

	if (nb_file_bytes == 0)
		return DOCA_ERROR_INVALID_VALUE;

	*file_size = (size_t)nb_file_bytes;
	return DOCA_SUCCESS;
}

/**
 * @brief Read file content into a pre-allocated buffer
 *
 * @param[in] path - Path to the file
 * @param[out] buffer - Pre-allocated buffer to store file content
 * @param[out] bytes_read - Number of bytes read from the file
 * @return DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t read_file_into_buffer(const char *path, char *buffer, size_t *bytes_read)
{
	FILE *file;
	size_t read_byte_count;

	file = fopen(path, "rb");
	if (file == NULL)
		return DOCA_ERROR_NOT_FOUND;

	read_byte_count = fread(buffer, 1, *bytes_read, file);
	fclose(file);

	if (read_byte_count != *bytes_read)
		return DOCA_ERROR_IO_FAILED;

	*bytes_read = read_byte_count;
	return DOCA_SUCCESS;
}

/*
 * Check if the provided device name is a name of a valid IB device
 *
 * @device_name [in]: The wanted IB device name
 * @role [in]: Role of the PCC context
 * @return: True if device_name is an IB device, false otherwise.
 */
static bool pcc_device_exists_check(const char *device_name)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	bool exists = false;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to load DOCA devices list: %s\n", doca_error_get_descr(result));
		return false;
	}

	/* Search device with same device name */
	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS)
			continue;

		/* Check if we found the device with the wanted name */
		if (strncmp(device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) == 0) {
			exists = true;
			break;
		}
	}

	doca_devinfo_destroy_list(dev_list);

	return exists;
}

/*
 * Open DOCA device that supports PCC
 *
 * @device_name [in]: Requested IB device name
 * @role [in]: Role of the PCC context
 * @doca_device [out]: An allocated DOCA device that supports PCC on success and NULL otherwise
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t open_pcc_device(const char *device_name, pcc_role_t role, struct doca_dev **doca_device)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to load DOCA devices list: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Search device with same device name and PCC capabilities supported */
	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: could not get DOCA device name\n");
			continue;
		}

		/* Check if the device has the requested device name */
		if (strncmp(device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) != 0)
			continue;

		if (role == PCC_ROLE_RP)
			result = doca_devinfo_get_is_pcc_supported(dev_list[i]);
		else if (role == PCC_ROLE_NP)
			result = doca_pcc_np_cap_is_supported(dev_list[i]);
		if (result != DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			PRINT_ERROR("Error: DOCA device %s does not support PCC %s role\n",
				    device_name,
				    (role == PCC_ROLE_RP ? "RP" : "NP"));
			return result;
		}

		result = doca_dev_open(dev_list[i], doca_device);
		if (result != DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			PRINT_ERROR("Error: Failed to open DOCA device: %s\n", doca_error_get_descr(result));
			return result;
		}
		break;
	}

	doca_devinfo_destroy_list(dev_list);

	if (*doca_device == NULL) {
		PRINT_ERROR("Error: Couldn't get DOCA device %s\n", device_name);
		return DOCA_ERROR_NOT_FOUND;
	}

	return result;
}

doca_error_t pcc_init(struct pcc_config *cfg, struct pcc_resources *resources)
{
	doca_error_t result, tmp_result;
	uint32_t min_num_threads, max_num_threads;

	/* Check if both threads list and DPA resources are specified */
	if (use_dpa_resources && use_threads_list) {
		PRINT_ERROR(
			"Error: Cannot specify both threads list and DPA resources. Use either threads list or DPA resources (with application key).\n");
		return DOCA_ERROR_BAD_CONFIG;
	}

	/* If DPA resources are specified, read the DPA resources file */
	if (use_dpa_resources) {
		char *file_buffer;
		size_t bytes_read;
		struct flexio_resource *res;

		/* Get the file size first */
		result = get_file_size(cfg->dpa_resources_file, &bytes_read);
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: Failed to get DPA resources file size: %s\n", doca_error_get_descr(result));
			return result;
		}

		/* Allocate buffer based on file size */
		file_buffer = (char *)malloc(bytes_read);
		if (file_buffer == NULL) {
			PRINT_ERROR("Error: Failed to allocate memory for DPA resources file\n");
			return DOCA_ERROR_NO_MEMORY;
		}

		/* Read the DPA resources file */
		result = read_file_into_buffer(cfg->dpa_resources_file, file_buffer, &bytes_read);
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: Failed to open DPA resources file: %s\n", doca_error_get_descr(result));
			free(file_buffer);
			return result;
		}

		// if app_key is not set, we will default to the pcc_app name
		const char *app_key = cfg->dpa_application_key;
		if (app_key == NULL || strlen(app_key) == 0) {
			struct flexio_app *pcc_app = (struct flexio_app *)(cfg->app);
			const char *app_name = flexio_app_get_name(pcc_app);
			app_key = app_name;
		}

		/* Create the DPA resources object */
		flexio_status res_created = flexio_resources_create(app_key, file_buffer, bytes_read, &res);
		if (res_created != FLEXIO_STATUS_SUCCESS) {
			PRINT_ERROR("Error: Failed creating DPA resources object!\n");
			free(file_buffer);
			return DOCA_ERROR_INITIALIZATION;
		}

		/* No support for eu groups yet */
		int num_eu_groups = flexio_resources_get_eugs_num(res);
		if (num_eu_groups > 0) {
			PRINT_ERROR("Error: Execution unit groups are currently unsupported!\n");
			free(file_buffer);
			flexio_resources_destroy(res);
			return DOCA_ERROR_NOT_SUPPORTED;
		}

		/* Get the number of execution units */
		uint32_t num_eus = flexio_resources_get_eus_num(res);
		uint32_t *eus = flexio_resources_get_eus(res);

		/* Print information about the execution units */
		PRINT_DEBUG("Debug: Found %d execution units in DPA resources file\n", num_eus);

		for (uint32_t i = 0; i < num_eus; i++) {
			cfg->threads_list[i] = eus[i];
		}
		cfg->threads_num = num_eus;

		flexio_resources_destroy(res);
		free(file_buffer);
	}

	/* Open DOCA device that supports PCC */
	result = open_pcc_device(cfg->device_name, cfg->role, &(resources->doca_device));
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to open DOCA device that supports PCC\n");
		return result;
	}

	/* Create DOCA PCC context */
	bool use_default_threads = !use_threads_list && !use_dpa_resources;
	if (cfg->role == PCC_ROLE_RP)
		result = doca_pcc_create(resources->doca_device, &(resources->doca_pcc));
	else if (cfg->role == PCC_ROLE_NP)
		result = doca_pcc_np_create(resources->doca_device, &(resources->doca_pcc));
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create DOCA PCC context\n");
		goto close_doca_dev;
	}

	/* Define default threads if not set according to role */
	if (use_default_threads) {
		if (cfg->role == PCC_ROLE_RP) {
			memcpy(cfg->threads_list, default_pcc_rp_threads_list, sizeof(default_pcc_rp_threads_list));
			cfg->threads_num = PCC_RP_THREADS_NUM_DEFAULT_VALUE;
		} else if (cfg->role == PCC_ROLE_NP) {
			memcpy(cfg->threads_list, default_pcc_np_threads_list, sizeof(default_pcc_np_threads_list));
			cfg->threads_num = PCC_NP_THREADS_NUM_DEFAULT_VALUE;
		}
	}

	result = doca_pcc_get_min_num_threads(resources->doca_pcc, &min_num_threads);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to get minimum DOCA PCC number of threads\n");
		goto destroy_pcc;
	}

	result = doca_pcc_get_max_num_threads(resources->doca_pcc, &max_num_threads);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Failed to get maximum DOCA PCC number of threads\n");
		goto destroy_pcc;
	}

	if (cfg->threads_num < min_num_threads || cfg->threads_num > max_num_threads) {
		PRINT_ERROR(
			"Invalid number of PCC threads: %u. The Minimum number of PCC threads is %d and the maximum number of PCC threads is %d\n",
			cfg->threads_num,
			min_num_threads,
			max_num_threads);
		result = DOCA_ERROR_INVALID_VALUE;
		goto destroy_pcc;
	}

	result = doca_pcc_set_app(resources->doca_pcc, cfg->app);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set DOCA PCC app\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC thread affinity */
	result = doca_pcc_set_thread_affinity(resources->doca_pcc, cfg->threads_num, cfg->threads_list);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set thread affinity for DOCA PCC\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC probe packet format */
	switch (cfg->probe_packet_format) {
	case PCC_DEV_PROBE_PACKET_CCMAD:
		result = doca_pcc_set_ccmad_probe_packet_format(resources->doca_pcc, 0);
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: Failed to set CCMAD probe packet format for DOCA PCC\n");
			goto destroy_pcc;
		}
		if (cfg->role == PCC_ROLE_RP) {
			result =
				doca_pcc_rp_set_ccmad_remote_sw_handler(resources->doca_pcc, 0, cfg->remote_sw_handler);
			if (result != DOCA_SUCCESS) {
				PRINT_ERROR("Error: Failed to set CCMAD remote SW handler for DOCA PCC\n");
				goto destroy_pcc;
			}
		}
		break;
	case PCC_DEV_PROBE_PACKET_IFA1:
		result = doca_pcc_set_ifa1_probe_packet_format(resources->doca_pcc, 0);
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: Failed to set IFA1 probe packet format for DOCA PCC\n");
			goto destroy_pcc;
		}
		break;
	case PCC_DEV_PROBE_PACKET_IFA2:
		result = doca_pcc_set_ifa2_probe_packet_format(resources->doca_pcc, 0);
		if (result != DOCA_SUCCESS) {
			PRINT_ERROR("Error: Failed to set IFA2 probe packet format for DOCA PCC\n");
			goto destroy_pcc;
		}
		if (cfg->role == PCC_ROLE_RP) {
			result = doca_pcc_rp_set_ifa2_gns(resources->doca_pcc, 0, cfg->gns);
			if (result != DOCA_SUCCESS) {
				PRINT_ERROR("Error: Failed to set IFA2 GNS for DOCA PCC\n");
				goto destroy_pcc;
			}
			result = doca_pcc_rp_set_ifa2_hop_limit(resources->doca_pcc, 0, cfg->hop_limit);
			if (result != DOCA_SUCCESS) {
				PRINT_ERROR("Error: Failed to set IFA2 hop limit for DOCA PCC\n");
				goto destroy_pcc;
			}
		} else if (cfg->role == PCC_ROLE_NP) {
			result = doca_pcc_np_set_ifa2_gns_ignore(resources->doca_pcc,
								 0,
								 cfg->gns_ignore_mask,
								 cfg->gns_ignore_value);
			if (result != DOCA_SUCCESS) {
				PRINT_ERROR("Error: Failed to set IFA2 GNS ignore for DOCA PCC\n");
				goto destroy_pcc;
			}
		}
		break;
	}

	/* Set DOCA PCC print buffer size */
	result = doca_pcc_set_print_buffer_size(resources->doca_pcc, PCC_PRINT_BUFFER_SIZE_DEFAULT_VALUE);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set print buffer size for DOCA PCC\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC trace message formats */
	result = doca_pcc_set_trace_message(resources->doca_pcc, trace_message_formats);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set trace message for DOCA PCC\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC coredump file pathname */
	result = doca_pcc_set_dev_coredump_file(resources->doca_pcc, cfg->coredump_file);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set coredump file for DOCA PCC\n");
		goto destroy_pcc;
	}

	/* Set DOCA PCC mailbox */
	result = doca_pcc_set_mailbox(resources->doca_pcc, PCC_MAILBOX_REQUEST_SIZE, PCC_MAILBOX_RESPONSE_SIZE);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to set mailbox for DOCA PCC\n");
		goto destroy_pcc;
	}

	return result;

destroy_pcc:
	tmp_result = doca_pcc_destroy(resources->doca_pcc);
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to destroy DOCA PCC context: %s\n", doca_error_get_descr(result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
close_doca_dev:
	tmp_result = doca_dev_close(resources->doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to close DOCA device: %s\n", doca_error_get_descr(result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t pcc_mailbox_send(struct pcc_config *cfg, struct pcc_resources *resources)
{
	doca_error_t result;
	uint32_t *request_buf;
	uint32_t response_size, cb_ret_val;

	if (!(cfg->app == pcc_np_switch_telemetry_app))
		return DOCA_SUCCESS;

	/* Get the request buffer of the mailbox */
	result = doca_pcc_mailbox_get_request_buffer(resources->doca_pcc, (void **)&request_buf);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to get the request buffer of the PCC mailbox\n");
		return result;
	}

	/* send hop limit to device */
	*request_buf = cfg->hop_limit;

	/* Send the request buffer that holds the hop limit */
	result = doca_pcc_mailbox_send(resources->doca_pcc, PCC_MAILBOX_REQUEST_SIZE, &response_size, &cb_ret_val);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to send the PCC mailbox request buffer\n");
		return result;
	}

	if (cb_ret_val != 0) {
		PRINT_ERROR("Error: Mailbox callback returned error status\n");
		return DOCA_ERROR_UNEXPECTED;
	}

	return DOCA_SUCCESS;
}

doca_error_t pcc_destroy(struct pcc_resources *resources)
{
	doca_error_t result, tmp_result;

	result = doca_pcc_destroy(resources->doca_pcc);
	if (result != DOCA_SUCCESS)
		PRINT_ERROR("Error: Failed to destroy DOCA PCC context: %s\n", doca_error_get_descr(result));

	tmp_result = doca_dev_close(resources->doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to close DOCA device: %s\n", doca_error_get_descr(result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

/*
 * ARGP Callback - Handle IB device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t device_name_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		PRINT_ERROR("Error: Entered IB device name exceeding the maximum size of %d\n",
			    DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(pcc_cfg->device_name, device_name, len + 1);

	if (!pcc_device_exists_check(pcc_cfg->device_name)) {
		PRINT_ERROR("Error: Entered IB device name: %s doesn't exist\n", pcc_cfg->device_name);
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC RP Switch Telemetry parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rp_switch_telemetry_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	bool rp_switch_telemetry = *((bool *)param);

	if (user_set_app) {
		PRINT_ERROR("Error: Can not set multiple runtime application.\n");
		return DOCA_ERROR_INITIALIZATION;
	}

	if (rp_switch_telemetry) {
		pcc_cfg->app = pcc_rp_switch_telemetry_app;
		pcc_cfg->probe_packet_format = PCC_DEV_PROBE_PACKET_IFA2;
	}
	user_set_app = true;
	PRINT_INFO("Info: Set DOCA PCC RP Switch Telemetry application\n");

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC NP Switch Telemetry parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t np_switch_telemetry_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	bool np_switch_telemetry = *((bool *)param);

	if (user_set_app) {
		PRINT_ERROR("Error: Can not set multiple runtime application.\n");
		return DOCA_ERROR_INITIALIZATION;
	}

	if (np_switch_telemetry) {
		pcc_cfg->app = pcc_np_switch_telemetry_app;
		pcc_cfg->role = PCC_ROLE_NP;
		pcc_cfg->probe_packet_format = PCC_DEV_PROBE_PACKET_IFA2;
	}
	user_set_app = true;
	PRINT_INFO("Info: Set DOCA PCC RP Switch Telemetry application\n");

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC threads list parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t threads_list_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	char *threads_list_string = (char *)param;
	static const char delim[2] = " ";
	char *curr_pcc_string;
	int curr_pcc_check, i, len;
	uint32_t curr_pcc;

	len = strnlen(threads_list_string, MAX_ARG_SIZE);
	if (len == MAX_ARG_SIZE) {
		PRINT_ERROR("Error: Entered PCC threads list exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	use_threads_list = true;
	pcc_cfg->threads_num = 0;

	/* Check and fill out the PCC threads list */
	/* Get the first PCC thread number */
	curr_pcc_string = strtok(threads_list_string, delim);
	if (curr_pcc_string == NULL) {
		PRINT_ERROR("Error: Invalid PCC threads list: %s\n", threads_list_string);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Walk through rest of the PCC threads numbers */
	while (curr_pcc_string != NULL) {
		/* Check if it's a number by checking its digits */
		len = strnlen(threads_list_string, MAX_ARG_SIZE);
		for (i = 0; i < len; i++) {
			if (!isdigit(curr_pcc_string[i])) {
				PRINT_ERROR("Error: Invalid PCC thread number: %s\n", curr_pcc_string);
				return DOCA_ERROR_INVALID_VALUE;
			}
		}

		/* Convert to integer to check if it is non-negative */
		curr_pcc_check = (int)atoi(curr_pcc_string);
		if (curr_pcc_check < 0) {
			PRINT_ERROR("Error: Invalid PCC thread number %d. PCC threads numbers must be non-negative\n",
				    curr_pcc_check);
			return DOCA_ERROR_INVALID_VALUE;
		}

		curr_pcc = (uint32_t)atoi(curr_pcc_string);
		pcc_cfg->threads_list[pcc_cfg->threads_num++] = curr_pcc;
		curr_pcc_string = strtok(NULL, delim);
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC wait time parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t wait_time_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	int wait_time = *((int *)param);

	/* Wait time must be either positive or infinity (meaning -1 )*/
	if (wait_time == 0) {
		PRINT_ERROR(
			"Error: Entered wait time can't be zero. Must be either positive or infinity (meaning negative value)\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	pcc_cfg->wait_time = wait_time;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC remote SW handler parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ccmad_remote_sw_handler_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;

	pcc_cfg->remote_sw_handler = *((bool *)param);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC hop limit parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ifa2_hop_limit_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;

	pcc_cfg->hop_limit = *((uint8_t *)param);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC gns parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ifa2_gns_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;

	pcc_cfg->gns = *((uint8_t *)param);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC gns ignore mask parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ifa2_gns_ignore_mask_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;

	pcc_cfg->gns_ignore_mask = *((uint8_t *)param);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC gns ignore value parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t ifa2_gns_ignore_value_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;

	pcc_cfg->gns_ignore_value = *((uint8_t *)param);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle PCC device coredump file parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t coredump_file_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	const char *path = (char *)param;

	int path_len = strnlen(path, MAX_ARG_SIZE);
	if (path_len == MAX_ARG_SIZE) {
		PRINT_ERROR("Entered path exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* The string will be '\0' terminated due to the strnlen check above */
	strncpy(pcc_cfg->coredump_file, path, path_len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handles DPA resources file path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpa_resources_file_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	const char *path = (char *)param;

	int path_len = strnlen(path, MAX_ARG_SIZE);
	if (path_len == MAX_ARG_SIZE) {
		PRINT_ERROR("Error: Entered path exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(pcc_cfg->dpa_resources_file, path, path_len + 1);

	/* Check if the DPA resources file exists */
	if (path_len > 0) {
		FILE *file = fopen(path, "r");
		if (file == NULL) {
			PRINT_ERROR("Error: DPA resources file '%s' does not exist or cannot be accessed\n", path);
			return DOCA_ERROR_NOT_FOUND;
		}
		fclose(file);
		use_dpa_resources = true;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handles DPA application key parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dpa_application_key_callback(void *param, void *config)
{
	struct pcc_config *pcc_cfg = (struct pcc_config *)config;
	const char *app_key = (char *)param;

	int dpa_app_key_len = strnlen(app_key, MAX_ARG_SIZE);
	if (dpa_app_key_len == MAX_ARG_SIZE) {
		PRINT_ERROR("Entered path exceeded buffer size: %d\n", MAX_USER_ARG_SIZE);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(pcc_cfg->dpa_application_key, app_key, dpa_app_key_len + 1);

	return DOCA_SUCCESS;
}

doca_error_t register_pcc_params(void)
{
	struct doca_argp_param *device_param;
	struct doca_argp_param *rp_switch_telemetry_param;
	struct doca_argp_param *np_switch_telemetry_param;
	struct doca_argp_param *threads_list_param;
	struct doca_argp_param *wait_time_param;
	struct doca_argp_param *remote_sw_handler_param;
	struct doca_argp_param *hop_limit_param;
	struct doca_argp_param *gns_param;
	struct doca_argp_param *gns_ignore_mask_param;
	struct doca_argp_param *gns_ignore_value_param;
	struct doca_argp_param *coredump_file_param;
	struct doca_argp_param *dpa_resources_file;
	struct doca_argp_param *dpa_application_key;

	/* Create and register DOCA device name parameter */
	doca_error_t result = doca_argp_param_create(&device_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(device_param, "d");
	doca_argp_param_set_long_name(device_param, "device");
	doca_argp_param_set_arguments(device_param, "<RDMA device names>");
	doca_argp_param_set_description(device_param, "RDMA device name that supports PCC (mandatory).");
	doca_argp_param_set_callback(device_param, device_name_callback);
	doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(device_param);
	result = doca_argp_register_param(device_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC RP Switch Telemetry parameter */
	result = doca_argp_param_create(&rp_switch_telemetry_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rp_switch_telemetry_param, "rp-st");
	doca_argp_param_set_long_name(rp_switch_telemetry_param, "rp-switch-telemetry");
	doca_argp_param_set_arguments(rp_switch_telemetry_param, "<PCC Reaction Point Switch Telemetry>");
	doca_argp_param_set_description(
		rp_switch_telemetry_param,
		"Flag to indicate running as a Reaction Point Switch Telemetry (optional). The application will generate IFA2 probe packets. By default the flag is set to false.");
	doca_argp_param_set_callback(rp_switch_telemetry_param, rp_switch_telemetry_callback);
	doca_argp_param_set_type(rp_switch_telemetry_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(rp_switch_telemetry_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC NP Switch Telemetry parameter */
	result = doca_argp_param_create(&np_switch_telemetry_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(np_switch_telemetry_param, "np-st");
	doca_argp_param_set_long_name(np_switch_telemetry_param, "np-switch-telemetry");
	doca_argp_param_set_arguments(np_switch_telemetry_param, "<PCC Notification Point Switch Telemetry>");
	doca_argp_param_set_description(
		np_switch_telemetry_param,
		"Flag to indicate running as a Notification Point Switch Telemetry (optional). The application will generate IFA2 probe packets. By default the flag is set to false.");
	doca_argp_param_set_callback(np_switch_telemetry_param, np_switch_telemetry_callback);
	doca_argp_param_set_type(np_switch_telemetry_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(np_switch_telemetry_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC threads list parameter */
	result = doca_argp_param_create(&threads_list_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(threads_list_param, "t");
	doca_argp_param_set_long_name(threads_list_param, "threads");
	doca_argp_param_set_arguments(threads_list_param, "<PCC threads list>");
	doca_argp_param_set_description(
		threads_list_param,
		"A list of the PCC threads numbers to be chosen for the DOCA PCC context to run on (optional). Must be provided as a string, such that the number are separated by a space.");
	doca_argp_param_set_callback(threads_list_param, threads_list_callback);
	doca_argp_param_set_type(threads_list_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(threads_list_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC wait time parameter */
	result = doca_argp_param_create(&wait_time_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(wait_time_param, "w");
	doca_argp_param_set_long_name(wait_time_param, "wait-time");
	doca_argp_param_set_arguments(wait_time_param, "<PCC wait time>");
	doca_argp_param_set_description(
		wait_time_param,
		"The duration of the DOCA PCC wait (optional), can provide negative values which means infinity. If not provided then -1 will be chosen.");
	doca_argp_param_set_callback(wait_time_param, wait_time_callback);
	doca_argp_param_set_type(wait_time_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(wait_time_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC remote SW handler */
	result = doca_argp_param_create(&remote_sw_handler_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(remote_sw_handler_param, "r-handler");
	doca_argp_param_set_long_name(remote_sw_handler_param, "remote-sw-handler");
	doca_argp_param_set_arguments(remote_sw_handler_param, "<CCMAD remote SW handler>");
	doca_argp_param_set_description(
		remote_sw_handler_param,
		"CCMAD remote SW handler flag (optional). If not provided then false will be chosen.");
	doca_argp_param_set_callback(remote_sw_handler_param, ccmad_remote_sw_handler_callback);
	doca_argp_param_set_type(remote_sw_handler_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(remote_sw_handler_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC hop limit parameter */
	result = doca_argp_param_create(&hop_limit_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(hop_limit_param, "hl");
	doca_argp_param_set_long_name(hop_limit_param, "hop-limit");
	doca_argp_param_set_arguments(hop_limit_param, "<IFA2 hop limit>");
	doca_argp_param_set_description(
		hop_limit_param,
		"The IFA2 probe packet hop limit (optional). If not provided then 0XFE will be chosen.");
	doca_argp_param_set_callback(hop_limit_param, ifa2_hop_limit_callback);
	doca_argp_param_set_type(hop_limit_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(hop_limit_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC gns parameter */
	result = doca_argp_param_create(&gns_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gns_param, "gns");
	doca_argp_param_set_long_name(gns_param, "global-namespace");
	doca_argp_param_set_arguments(gns_param, "<IFA2 global namespace>");
	doca_argp_param_set_description(
		gns_param,
		"The IFA2 probe packet global namespace (optional). If not provided then 0XF will be chosen.");
	doca_argp_param_set_callback(gns_param, ifa2_gns_callback);
	doca_argp_param_set_type(gns_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(gns_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC gns ignore mask parameter */
	result = doca_argp_param_create(&gns_ignore_mask_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gns_ignore_mask_param, "gns-ignore-mask");
	doca_argp_param_set_long_name(gns_ignore_mask_param, "global-namespace-ignore-mask");
	doca_argp_param_set_arguments(gns_ignore_mask_param, "<IFA2 global namespace ignore mask>");
	doca_argp_param_set_description(
		gns_ignore_mask_param,
		"The IFA2 probe packet global namespace ignore mask (optional). If not provided then 0 will be chosen.");
	doca_argp_param_set_callback(gns_ignore_mask_param, ifa2_gns_ignore_mask_callback);
	doca_argp_param_set_type(gns_ignore_mask_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(gns_ignore_mask_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC gns ignore value parameter */
	result = doca_argp_param_create(&gns_ignore_value_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gns_ignore_value_param, "gns-ignore-val");
	doca_argp_param_set_long_name(gns_ignore_value_param, "global-namespace-ignore-value");
	doca_argp_param_set_arguments(gns_ignore_value_param, "<IFA2 global namespace ignore value>");
	doca_argp_param_set_description(
		gns_ignore_value_param,
		"The IFA2 probe packet global namespace ignore value (optional). If not provided then 0 will be chosen.");
	doca_argp_param_set_callback(gns_ignore_value_param, ifa2_gns_ignore_value_callback);
	doca_argp_param_set_type(gns_ignore_value_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(gns_ignore_value_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register PCC device coredump file parameter */
	result = doca_argp_param_create(&coredump_file_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(coredump_file_param, "f");
	doca_argp_param_set_long_name(coredump_file_param, "coredump-file");
	doca_argp_param_set_arguments(coredump_file_param, "<PCC coredump file>");
	doca_argp_param_set_description(
		coredump_file_param,
		"A pathname to the file to write coredump data in case of unrecoverable error on the device (optional). Must be provided as a string.");
	doca_argp_param_set_callback(coredump_file_param, coredump_file_callback);
	doca_argp_param_set_type(coredump_file_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(coredump_file_param);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register DPA application name parameter */
	result = doca_argp_param_create(&dpa_application_key);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(dpa_application_key, "dpa-app-key");
	doca_argp_param_set_arguments(dpa_application_key, "<DPA application key>");
	doca_argp_param_set_description(dpa_application_key, "Application key in specified DPA resources .yaml file");
	doca_argp_param_set_callback(dpa_application_key, dpa_application_key_callback);
	doca_argp_param_set_type(dpa_application_key, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dpa_application_key);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	/* Create and register DPA resources file parameter */
	result = doca_argp_param_create(&dpa_resources_file);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(dpa_resources_file, "dpa-resources");
	doca_argp_param_set_arguments(dpa_resources_file, "<DPA resources file>");
	doca_argp_param_set_description(dpa_resources_file, "Path to a DPA resources .yaml file");
	doca_argp_param_set_callback(dpa_resources_file, dpa_resources_file_callback);
	doca_argp_param_set_type(dpa_resources_file, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dpa_resources_file);
	if (result != DOCA_SUCCESS) {
		PRINT_ERROR("Error: failed to register program param: %s\n", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}
