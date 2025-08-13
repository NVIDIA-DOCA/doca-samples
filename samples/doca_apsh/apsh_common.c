/*
 * Copyright (c) 2022 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <doca_argp.h>
#include <doca_log.h>
#include <doca_apsh.h>
#include <doca_apsh_attr.h>

#include "common.h"
#include "apsh_common.h"

DOCA_LOG_REGISTER(APSH_COMMON);

static struct doca_dev *dma_device;
static struct doca_dev_rep *pci_device;

/*
 * ARGP Callback - Handle PID parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pid_callback(void *param, void *config)
{
	struct apsh_config *conf = (struct apsh_config *)config;

	conf->pid = *(DOCA_APSH_PROCESS_PID_TYPE *)param;
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle VUID parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vuid_callback(void *param, void *config)
{
	struct apsh_config *conf = (struct apsh_config *)config;
	size_t size = sizeof(conf->system_vuid);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("System VUID argument too long. Must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->system_vuid, param);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle DMA device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dma_callback(void *param, void *config)
{
	struct apsh_config *conf = (struct apsh_config *)config;
	size_t size = sizeof(conf->dma_dev_name);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("DMA device name argument too long. Must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->dma_dev_name, param);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle OS Type parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t os_type_callback(void *param, void *config)
{
	struct apsh_config *conf = (struct apsh_config *)config;
	char *str_param = (char *)param;

	if (!strcasecmp(str_param, "windows"))
		conf->os_type = DOCA_APSH_SYSTEM_WINDOWS;
	else if (!strcasecmp(str_param, "linux"))
		conf->os_type = DOCA_APSH_SYSTEM_LINUX;
	else {
		DOCA_LOG_ERR("OS type is not windows/linux (case insensitive)");
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle mem_regions.json path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t memr_callback(void *param, void *config)
{
	struct apsh_config *conf = (struct apsh_config *)config;
	size_t size = sizeof(conf->system_mem_region_path);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("System memory regions map argument too long, must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->system_mem_region_path, param);

	if (access(conf->system_mem_region_path, F_OK) == -1) {
		DOCA_LOG_ERR("System memory regions map json file not found %s", conf->system_mem_region_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle os_symbols.json path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t os_syms_callback(void *param, void *config)
{
	struct apsh_config *conf = (struct apsh_config *)config;
	size_t size = sizeof(conf->system_os_symbol_map_path);

	if (strnlen(param, size) >= size) {
		DOCA_LOG_ERR("System os symbols map argument too long, must be <=%zu long", size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strcpy(conf->system_os_symbol_map_path, param);

	if (access(conf->system_os_symbol_map_path, F_OK) == -1) {
		DOCA_LOG_ERR("System os symbols map json file not found %s", conf->system_os_symbol_map_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	return DOCA_SUCCESS;
}

doca_error_t register_apsh_params(bool add_os_arg, bool add_pid_arg)
{
	doca_error_t result;
	struct doca_argp_param *pid_param, *vuid_param, *dma_param, *os_type_param, *memr_param, *os_syms_param;

	/* Create and register pid param */
	if (!add_pid_arg)
		goto skip_pid;
	result = doca_argp_param_create(&pid_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pid_param, "p");
	doca_argp_param_set_long_name(pid_param, "pid");
	doca_argp_param_set_description(pid_param, "Process ID of process to be analyzed");
	doca_argp_param_set_callback(pid_param, pid_callback);
	doca_argp_param_set_type(pid_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(pid_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

skip_pid:
	/* Create and register VUID param */
	result = doca_argp_param_create(&vuid_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(vuid_param, "f");
	doca_argp_param_set_long_name(vuid_param, "vuid");
	doca_argp_param_set_description(vuid_param, "VUID of the System device");
	doca_argp_param_set_callback(vuid_param, vuid_callback);
	doca_argp_param_set_type(vuid_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(vuid_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register DMA param */
	result = doca_argp_param_create(&dma_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(dma_param, "d");
	doca_argp_param_set_long_name(dma_param, "dma");
	doca_argp_param_set_description(dma_param, "DMA device name");
	doca_argp_param_set_callback(dma_param, dma_callback);
	doca_argp_param_set_type(dma_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dma_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register system OS type param */
	if (!add_os_arg)
		return DOCA_SUCCESS;
	result = doca_argp_param_create(&os_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(os_type_param, "s");
	doca_argp_param_set_long_name(os_type_param, "osty");
	doca_argp_param_set_arguments(os_type_param, "<windows|linux>");
	doca_argp_param_set_description(os_type_param, "System OS type - windows/linux");
	doca_argp_param_set_callback(os_type_param, os_type_callback);
	doca_argp_param_set_type(os_type_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(os_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register system memory map param */
	result = doca_argp_param_create(&memr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(memr_param, "m");
	doca_argp_param_set_long_name(memr_param, "memr");
	doca_argp_param_set_arguments(memr_param, "<path>");
	doca_argp_param_set_description(memr_param, "System memory regions map");
	doca_argp_param_set_callback(memr_param, memr_callback);
	doca_argp_param_set_type(memr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(memr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register system OS map param */
	result = doca_argp_param_create(&os_syms_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(os_syms_param, "o");
	doca_argp_param_set_long_name(os_syms_param, "osym");
	doca_argp_param_set_arguments(os_syms_param, "<path>");
	doca_argp_param_set_description(os_syms_param, "System OS symbol map path");
	doca_argp_param_set_callback(os_syms_param, os_syms_callback);
	doca_argp_param_set_type(os_syms_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(os_syms_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

doca_error_t init_doca_apsh(const char *dma_device_name, struct doca_apsh_ctx **ctx)
{
	doca_error_t result;
	struct doca_apsh_ctx *apsh_ctx;

	/* Get dma device */
	result = open_doca_device_with_ibdev_name((const uint8_t *)dma_device_name,
						  strlen(dma_device_name),
						  NULL,
						  &dma_device);
	if (result != DOCA_SUCCESS)
		return result;

	/* Init apsh library */
	apsh_ctx = doca_apsh_create();
	if (apsh_ctx == NULL) {
		doca_dev_close(dma_device);
		return DOCA_ERROR_NO_MEMORY;
	}

	/* set the DMA device */
	result = doca_apsh_dma_dev_set(apsh_ctx, dma_device);
	if (result != DOCA_SUCCESS) {
		cleanup_doca_apsh(apsh_ctx, NULL);
		return result;
	}

	/* Start apsh context */
	result = doca_apsh_start(apsh_ctx);
	if (result != DOCA_SUCCESS) {
		cleanup_doca_apsh(apsh_ctx, NULL);
		return result;
	}

	*ctx = apsh_ctx;
	return DOCA_SUCCESS;
}

doca_error_t init_doca_apsh_system(struct doca_apsh_ctx *ctx,
				   enum doca_apsh_system_os os_type,
				   const char *os_symbols,
				   const char *mem_region,
				   const char *pci_vuid,
				   struct doca_apsh_system **system)
{
	doca_error_t result;
	struct doca_apsh_system *sys = NULL;

	/* Get pci device that connects to the system */
	result = open_doca_device_rep_with_vuid(dma_device,
						DOCA_DEVINFO_REP_FILTER_NET,
						(const uint8_t *)pci_vuid,
						strlen(pci_vuid),
						&pci_device);
	if (result != DOCA_SUCCESS)
		goto err;

	/* Create a new system handler to introspect */
	sys = doca_apsh_system_create(ctx);
	if (sys == NULL) {
		cleanup_doca_apsh(ctx, NULL);
		return DOCA_ERROR_INITIALIZATION;
	}

	/* Set the system os type - linux/widows */
	result = doca_apsh_sys_os_type_set(sys, os_type);
	if (result != DOCA_SUCCESS)
		goto err;

	/* Check if os_symbols is a file or directory */
	struct stat path_stat;
	if (stat(os_symbols, &path_stat) != 0) {
		DOCA_LOG_ERR("Failed to stat os_symbols path: %s", os_symbols);
		result = DOCA_ERROR_IO_FAILED;
		goto err;
	}

	/* Set the system os symbol map */
	if (S_ISDIR(path_stat.st_mode)) {
		DOCA_LOG_TRC("Setting os_symbols from directory: %s", os_symbols);
		result = doca_apsh_sys_os_symbol_map_folder_set(sys, os_symbols);
	} else {
		DOCA_LOG_TRC("Setting os_symbols from file: %s", os_symbols);
		result = doca_apsh_sys_os_symbol_map_set(sys, os_symbols);
	}
	if (result != DOCA_SUCCESS)
		goto err;

	/* Set the system memory region the apsh handler is allowed to access */
	result = doca_apsh_sys_mem_region_set(sys, mem_region);
	if (result != DOCA_SUCCESS)
		goto err;

	/* Set the system device for the apsh library to use */
	result = doca_apsh_sys_dev_set(sys, pci_device);
	if (result != DOCA_SUCCESS)
		goto err;

	/* Start system handler and init connection to the system and the devices */
	result = doca_apsh_system_start(sys);
	if (result != DOCA_SUCCESS)
		goto err;

	*system = sys;
	return DOCA_SUCCESS;

err:
	cleanup_doca_apsh(ctx, sys);
	return result;
}

doca_error_t cleanup_doca_apsh(struct doca_apsh_ctx *ctx, struct doca_apsh_system *system)
{
	doca_apsh_destroy(ctx);
	if (system != NULL)
		doca_apsh_system_destroy(system);
	if (pci_device != NULL)
		doca_dev_rep_close(pci_device);
	if (dma_device != NULL)
		doca_dev_close(dma_device);

	return DOCA_SUCCESS;
}

doca_error_t process_get(DOCA_APSH_PROCESS_PID_TYPE pid,
			 struct doca_apsh_system *sys,
			 int *nb_procs,
			 struct doca_apsh_process ***processes,
			 struct doca_apsh_process **process)
{
	struct doca_apsh_process **pslist;
	int num_processes, i;
	doca_error_t result;

	*process = NULL;

	/* Read host processes list */
	result = doca_apsh_processes_get(sys, &pslist, &num_processes);
	if (result != DOCA_SUCCESS)
		return result;

	/* Search for the requested process */
	for (i = 0; i < num_processes; ++i) {
		if (doca_apsh_process_info_get(pslist[i], DOCA_APSH_PROCESS_PID) == pid) {
			*process = pslist[i];
			break;
		}
	}
	if (*process == NULL) {
		doca_apsh_processes_free(pslist);
		return DOCA_ERROR_NOT_FOUND;
	}

	/* Should not release the pslist, it will also release the memory of the requested process (with "pid") */
	*processes = pslist;
	*nb_procs = num_processes;
	return DOCA_SUCCESS;
}
