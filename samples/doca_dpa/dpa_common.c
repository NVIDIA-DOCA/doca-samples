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

#include "dpa_common.h"
#include "doca_dpa.h"
#include "libflexio/flexio_ver.h"

#define DPA_BASIC_INITIATOR_TARGET_FLEXIO_MAJOR_VERSION (25)
#define DPA_BASIC_INITIATOR_TARGET_FLEXIO_MINOR_VERSION (4)
#define DPA_BASIC_INITIATOR_TARGET_FLEXIO_PATCH_VERSION (0)

#define FLEXIO_VER_USED \
	FLEXIO_VER(DPA_BASIC_INITIATOR_TARGET_FLEXIO_MAJOR_VERSION, \
		   DPA_BASIC_INITIATOR_TARGET_FLEXIO_MINOR_VERSION, \
		   DPA_BASIC_INITIATOR_TARGET_FLEXIO_PATCH_VERSION)
#include "libflexio/flexio.h"

DOCA_LOG_REGISTER(DPA_COMMON);

#define DPA_THREADS_MAX 256

/*
 * A struct that includes all needed info on registered kernels and is initialized during linkage by DPACC.
 * Variable name should be the token passed to DPACC with --app-name parameter.
 */
extern struct doca_dpa_app *dpa_sample_app;

/*
 * ARGP Callback - Handle PF device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pf_device_name_param_callback(void *param, void *config)
{
	struct dpa_config *dpa_cfg = (struct dpa_config *)config;
	char *device_name = (char *)param;

	int len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered device name exceeding the maximum size of %d", DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(dpa_cfg->pf_device_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

static doca_error_t dpa_resources_file_param_callback(void *param, void *config)
{
	struct dpa_config *dpa_cfg = (struct dpa_config *)config;
	char *dpa_resources_file = (char *)param;

	int path_len = strnlen(dpa_resources_file, DPA_RESOURCES_PATH_MAX_SIZE);
	if (path_len >= DPA_RESOURCES_PATH_MAX_SIZE) {
		DOCA_LOG_ERR("Entered DPA resources file path exceeding the maximum size of %d",
			     DPA_RESOURCES_PATH_MAX_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(dpa_cfg->dpa_resources_file, dpa_resources_file, path_len + 1);

	return DOCA_SUCCESS;
}

static doca_error_t dpa_app_key_param_callback(void *param, void *config)
{
	struct dpa_config *dpa_cfg = (struct dpa_config *)config;
	const char *path = (char *)param;
	int dpa_app_key_len = strnlen(path, DPA_APP_KEY_MAX_SIZE);
	if (dpa_app_key_len >= DPA_APP_KEY_MAX_SIZE) {
		DOCA_LOG_ERR("Entered DPA application key exceeding the maximum size of %d", DPA_APP_KEY_MAX_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(dpa_cfg->dpa_app_key, path, dpa_app_key_len + 1);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle RDMA device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdma_device_name_param_callback(void *param, void *config)
{
	struct dpa_config *dpa_cfg = (struct dpa_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered device name exceeding the maximum size of %d", DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(dpa_cfg->rdma_device_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle gid_index parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t gid_index_param_callback(void *param, void *config)
{
	struct dpa_config *dpa_cfg = (struct dpa_config *)config;
	const int gid_index = *(uint32_t *)param;

	if (gid_index < 0) {
		DOCA_LOG_ERR("GID index for DOCA RDMA must be non-negative");
		return DOCA_ERROR_INVALID_VALUE;
	}

	dpa_cfg->is_gid_index_set = true;
	dpa_cfg->gid_index = (uint32_t)gid_index;

	return DOCA_SUCCESS;
}

doca_error_t register_dpa_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pf_device_param;
	struct doca_argp_param *dpa_resources_file_param;
	struct doca_argp_param *dpa_app_key_param;
	struct doca_argp_param *gid_index_param;

	result = doca_argp_param_create(&pf_device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pf_device_param, "pf_dev");
	doca_argp_param_set_long_name(pf_device_param, "pf-device");
	doca_argp_param_set_arguments(pf_device_param, "<PF DOCA device name>");
	doca_argp_param_set_description(
		pf_device_param,
		"PF device name that supports DPA (optional). If not provided then a random device will be chosen");
	doca_argp_param_set_callback(pf_device_param, pf_device_name_param_callback);
	doca_argp_param_set_type(pf_device_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(pf_device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&dpa_resources_file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(dpa_resources_file_param, "dpa-resources");
	doca_argp_param_set_arguments(dpa_resources_file_param, "<DPA resources file>");
	doca_argp_param_set_description(dpa_resources_file_param, "Path to a DPA resources .yaml file");
	doca_argp_param_set_callback(dpa_resources_file_param, dpa_resources_file_param_callback);
	doca_argp_param_set_type(dpa_resources_file_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dpa_resources_file_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&dpa_app_key_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(dpa_app_key_param, "dpa-app-key");
	doca_argp_param_set_arguments(dpa_app_key_param, "<DPA application key>");
	doca_argp_param_set_description(dpa_app_key_param, "Application key in specified DPA resources .yaml file");
	doca_argp_param_set_callback(dpa_app_key_param, dpa_app_key_param_callback);
	doca_argp_param_set_type(dpa_app_key_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(dpa_app_key_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	struct doca_argp_param *rdma_device_param;
	result = doca_argp_param_create(&rdma_device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rdma_device_param, "rdma_dev");
	doca_argp_param_set_long_name(rdma_device_param, "rdma-device");
	doca_argp_param_set_arguments(rdma_device_param, "<RDMA DOCA device name>");
	doca_argp_param_set_description(
		rdma_device_param,
		"device name that supports RDMA (optional). If not provided then a random device will be chosen");
	doca_argp_param_set_callback(rdma_device_param, rdma_device_name_param_callback);
	doca_argp_param_set_type(rdma_device_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(rdma_device_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(gid_index_param, "g");
	doca_argp_param_set_long_name(gid_index_param, "gid-index");
	doca_argp_param_set_description(gid_index_param, "GID index for DOCA RDMA (optional)");
	doca_argp_param_set_callback(gid_index_param, gid_index_param_callback);
	doca_argp_param_set_type(gid_index_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(gid_index_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Open DPA DOCA devices
 *
 * @pf_device_name [in]: Wanted PF device name, can be NOT_SET and then a random DPA supported device is chosen
 * @rdma_device_name [in]: Wanted RDMA device name, can be NOT_SET and then a random RDMA supported device is chosen
 * @pf_doca_device [out]: An allocated PF DOCA device on success and NULL otherwise
 * @rdma_doca_device [out]: An allocated RDMA DOCA device on success and NULL otherwise
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t open_dpa_device(const char *pf_device_name,
				    const char *rdma_device_name,
				    struct doca_dev **pf_doca_device,
				    struct doca_dev **rdma_doca_device)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result, dpa_cap;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	char opened_pf_device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;
	enum doca_pci_func_type func_type;
	uint8_t must_extend_pf_device = 0;
	*pf_doca_device = NULL;
	*rdma_doca_device = NULL;

	if (strcmp(pf_device_name, DEVICE_DEFAULT_NAME) != 0 && strcmp(rdma_device_name, DEVICE_DEFAULT_NAME) != 0 &&
	    strcmp(pf_device_name, rdma_device_name) == 0) {
		DOCA_LOG_ERR("RDMA DOCA device must be different than PF DOCA device (%s)", pf_device_name);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return result;
	}

	/* Start with opening PF DOCA device */
	for (i = 0; i < nb_devs; i++) {
		dpa_cap = doca_dpa_cap_is_supported(dev_list[i]);
		if (dpa_cap != DOCA_SUCCESS) {
			continue;
		}

		result = doca_devinfo_get_pci_func_type(dev_list[i], &func_type);
		if (result != DOCA_SUCCESS || func_type != DOCA_PCI_FUNC_TYPE_PF) {
			continue;
		}

		/* If device support network representors, then device must be extended */
		result = doca_devinfo_rep_cap_is_filter_net_supported(dev_list[i], &must_extend_pf_device);
		if (result != DOCA_SUCCESS) {
			continue;
		}

		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS) {
			continue;
		}

		if (strcmp(pf_device_name, DEVICE_DEFAULT_NAME) == 0 || strcmp(pf_device_name, ibdev_name) == 0) {
			result = doca_dev_open(dev_list[i], pf_doca_device);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to open DOCA device %s: %s",
					     ibdev_name,
					     doca_error_get_descr(result));
				goto exit_open_dpa_device;
			}

			strncpy(opened_pf_device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
			break;
		}
	}

	if (*pf_doca_device == NULL) {
		DOCA_LOG_ERR("Couldn't find PF DOCA device");
		result = DOCA_ERROR_NOT_FOUND;
		goto exit_open_dpa_device;
	}

	if (!must_extend_pf_device && strcmp(rdma_device_name, DEVICE_DEFAULT_NAME) != 0) {
		doca_dev_close(*pf_doca_device);
		DOCA_LOG_ERR("RDMA DOCA device is provided, but PF DOCA device is not extendable");
		result = DOCA_ERROR_BAD_CONFIG;
		goto exit_open_dpa_device;
	}

	/* In case pf device is not extended, then rdma device is equal to pf device */
	if (!must_extend_pf_device) {
		*rdma_doca_device = *pf_doca_device;
		goto exit_open_dpa_device;
	}

	for (i = 0; i < nb_devs; i++) {
		dpa_cap = doca_dpa_cap_is_supported(dev_list[i]);
		if (dpa_cap != DOCA_SUCCESS) {
			continue;
		}

		doca_error_t rdma_cap = doca_rdma_cap_task_send_is_supported(dev_list[i]);
		if (rdma_cap != DOCA_SUCCESS) {
			continue;
		}

		/* Can't use same rdma device as pf device (in case pf device is extended) */
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS || strcmp(ibdev_name, opened_pf_device_name) == 0) {
			continue;
		}

		if (strcmp(rdma_device_name, DEVICE_DEFAULT_NAME) == 0 || strcmp(rdma_device_name, ibdev_name) == 0) {
			result = doca_dev_open(dev_list[i], rdma_doca_device);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to open DOCA device %s: %s",
					     ibdev_name,
					     doca_error_get_descr(result));
				goto exit_open_dpa_device;
			}

			break;
		}
	}

	if (*rdma_doca_device == NULL) {
		DOCA_LOG_ERR("Couldn't find RDMA DOCA device");
		result = DOCA_ERROR_NOT_FOUND;
		goto exit_open_dpa_device;
	}

exit_open_dpa_device:

	if (result != DOCA_SUCCESS) {
		if (*pf_doca_device != NULL)
			doca_dev_close(*pf_doca_device);

		if (*rdma_doca_device != NULL)
			doca_dev_close(*rdma_doca_device);
	}

	doca_devinfo_destroy_list(dev_list);

	return result;
}

doca_error_t create_doca_dpa_wait_sync_event(struct doca_dpa *doca_dpa,
					     struct doca_dev *doca_device,
					     struct doca_sync_event **wait_event)
{
	doca_error_t result, tmp_result;

	result = doca_sync_event_create(wait_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_add_publisher_location_cpu(*wait_event, doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CPU as publisher for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_wait_event;
	}

	result = doca_sync_event_add_subscriber_location_dpa(*wait_event, doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA as subscriber for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_wait_event;
	}

	result = doca_sync_event_start(*wait_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_wait_event;
	}

	return result;

destroy_wait_event:
	tmp_result = doca_sync_event_destroy(*wait_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

doca_error_t create_doca_dpa_completion_sync_event(struct doca_dpa *doca_dpa,
						   struct doca_dev *doca_device,
						   struct doca_sync_event **comp_event,
						   doca_dpa_dev_sync_event_t *handle)
{
	doca_error_t result, tmp_result;

	result = doca_sync_event_create(comp_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_add_publisher_location_dpa(*comp_event, doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA as publisher for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	result = doca_sync_event_add_subscriber_location_cpu(*comp_event, doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CPU as subscriber for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	result = doca_sync_event_start(*comp_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	if (handle != NULL) {
		result = doca_sync_event_get_dpa_handle(*comp_event, doca_dpa, handle);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_sync_event_get_dpa_handle failed (%d)", result);
			goto destroy_comp_event;
		}
	}

	return result;

destroy_comp_event:
	tmp_result = doca_sync_event_destroy(*comp_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

doca_error_t create_doca_dpa_kernel_sync_event(struct doca_dpa *doca_dpa, struct doca_sync_event **kernel_event)
{
	doca_error_t result, tmp_result;

	result = doca_sync_event_create(kernel_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_add_publisher_location_dpa(*kernel_event, doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA as publisher for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_kernel_event;
	}

	result = doca_sync_event_add_subscriber_location_dpa(*kernel_event, doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA as subscriber for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_kernel_event;
	}

	result = doca_sync_event_start(*kernel_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_kernel_event;
	}

	return result;

destroy_kernel_event:
	tmp_result = doca_sync_event_destroy(*kernel_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

doca_error_t create_doca_remote_net_sync_event(struct doca_dev *doca_device, struct doca_sync_event **remote_net_event)
{
	doca_error_t result, tmp_result;

	result = doca_sync_event_create(remote_net_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_add_publisher_location_remote_net(*remote_net_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set remote net as publisher for DOCA sync event: %s",
			     doca_error_get_descr(result));
		goto destroy_remote_net_event;
	}

	result = doca_sync_event_add_subscriber_location_cpu(*remote_net_event, doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CPU as subscriber for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_remote_net_event;
	}

	result = doca_sync_event_start(*remote_net_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_remote_net_event;
	}

	return result;

destroy_remote_net_event:
	tmp_result = doca_sync_event_destroy(*remote_net_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

doca_error_t export_doca_remote_net_sync_event_to_dpa(struct doca_dev *doca_device,
						      struct doca_dpa *doca_dpa,
						      struct doca_sync_event *remote_net_event,
						      struct doca_sync_event_remote_net **remote_net_exported_event,
						      doca_dpa_dev_sync_event_remote_net_t *remote_net_event_dpa_handle)
{
	doca_error_t result, tmp_result;
	const uint8_t *remote_net_event_export_data;
	size_t remote_net_event_export_size;

	result = doca_sync_event_export_to_remote_net(remote_net_event,
						      &remote_net_event_export_data,
						      &remote_net_event_export_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to export DOCA sync event to remote net: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_remote_net_create_from_export(doca_device,
							       remote_net_event_export_data,
							       remote_net_event_export_size,
							       remote_net_exported_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create remote net DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_remote_net_get_dpa_handle(*remote_net_exported_event,
							   doca_dpa,
							   remote_net_event_dpa_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to export remote net DOCA sync event to DPA: %s", doca_error_get_descr(result));
		goto destroy_export_remote_net_event;
	}

	return result;

destroy_export_remote_net_event:
	tmp_result = doca_sync_event_remote_net_destroy(*remote_net_exported_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy remote net DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

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

/**
 * @brief Get execution unit IDs from FlexIO resources
 *
 * This function reads the DPA resources file and extracts the execution unit IDs
 * for the specified application key.
 *
 * @cfg [in]: DPA configuration
 * @threads_list [out]: List of execution unit IDs
 * @threads_num [out]: Number of execution units
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_eu_ids_from_resources_file(struct dpa_config *cfg,
						   uint32_t *threads_list,
						   uint32_t *threads_num)
{
	char *file_buffer;
	size_t bytes_read;
	struct flexio_resource *res;

	/* Get the file size first */
	doca_error_t result = get_file_size(cfg->dpa_resources_file, &bytes_read);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_DBG("DPA resources file not found: %s", doca_error_get_descr(result));
		return result;
	}

	/* Allocate buffer based on file size */
	file_buffer = (char *)malloc(bytes_read);
	if (file_buffer == NULL) {
		DOCA_LOG_ERR("Error: Failed to allocate memory for DPA resources file");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Read the DPA resources file */
	result = read_file_into_buffer(cfg->dpa_resources_file, file_buffer, &bytes_read);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Error: Failed to open DPA resources file: %s", doca_error_get_descr(result));
		free(file_buffer);
		return result;
	}

	const char *app_key = cfg->dpa_app_key;
	flexio_status res_created = flexio_resources_create(app_key, file_buffer, bytes_read, &res);
	if (res_created != FLEXIO_STATUS_SUCCESS) {
		DOCA_LOG_ERR("Error: Failed creating DPA resources object!");
		free(file_buffer);
		return DOCA_ERROR_BAD_CONFIG;
	}
	/* No support for eu groups yet */
	int num_eu_groups = flexio_resources_get_eugs_num(res);
	if (num_eu_groups > 0) {
		DOCA_LOG_ERR("Execution unit groups are currently unsupported!");
		free(file_buffer);
		flexio_resources_destroy(res);
		return DOCA_ERROR_BAD_CONFIG;
	}
	/* Get the number of execution units */
	uint32_t num_eus = flexio_resources_get_eus_num(res);
	uint32_t *eus = flexio_resources_get_eus(res);
	/* Print information about the execution units */
	DOCA_LOG_INFO("Info: Found %d execution units in DPA resources file", num_eus);
	for (uint32_t i = 0; i < num_eus; i++) {
		threads_list[i] = eus[i];
	}
	*threads_num = num_eus;
	flexio_resources_destroy(res);
	free(file_buffer);
	return DOCA_SUCCESS;
}

doca_error_t allocate_dpa_resources(struct dpa_config *cfg, struct dpa_resources *resources)
{
	doca_error_t result;
	uint32_t threads_list[DPA_THREADS_MAX] = {0};
	uint32_t threads_num = 0;
	doca_error_t get_execution_ids_status;

	resources->gid_index = cfg->gid_index;
	resources->is_gid_index_set = cfg->is_gid_index_set;

	result = open_dpa_device(cfg->pf_device_name,
				 cfg->rdma_device_name,
				 &resources->pf_doca_device,
				 &resources->rdma_doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function open_doca_device() failed");
		goto exit_label;
	}

	result = doca_dpa_create(resources->pf_doca_device, &(resources->pf_dpa_ctx));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA DPA context: %s", doca_error_get_descr(result));
		goto close_doca_dev;
	}

	result = doca_dpa_set_app(resources->pf_dpa_ctx, dpa_sample_app);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DOCA DPA app: %s", doca_error_get_descr(result));
		goto destroy_doca_dpa;
	}

	result = doca_dpa_start(resources->pf_dpa_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA DPA context: %s", doca_error_get_descr(result));
		goto destroy_doca_dpa;
	}

	if (resources->rdma_doca_device != resources->pf_doca_device) {
		result = doca_dpa_device_extend(resources->pf_dpa_ctx,
						resources->rdma_doca_device,
						&resources->rdma_dpa_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to extend DOCA DPA context: %s", doca_error_get_descr(result));
			goto destroy_doca_dpa;
		}

		result = doca_dpa_get_dpa_handle(resources->rdma_dpa_ctx, &resources->rdma_dpa_ctx_handle);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get DOCA DPA context handle: %s", doca_error_get_descr(result));
			goto destroy_rdma_doca_dpa;
		}
	} else {
		resources->rdma_dpa_ctx = resources->pf_dpa_ctx;
	}

	get_execution_ids_status = get_eu_ids_from_resources_file(cfg, threads_list, &threads_num);

	if (get_execution_ids_status == DOCA_SUCCESS && threads_num > 0) {
		resources->affinities = malloc(threads_num * sizeof(struct doca_dpa_eu_affinity *));
		if (resources->affinities == NULL) {
			DOCA_LOG_ERR("Failed to allocate memory for affinities");
			goto destroy_doca_dpa;
		}
		resources->num_affinities = threads_num;

		for (uint32_t i = 0; i < threads_num; ++i) {
			result = doca_dpa_eu_affinity_create(resources->rdma_dpa_ctx, &(resources->affinities[i]));
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Function doca_dpa_eu_affinity_create failed (%s)",
					     doca_error_get_descr(result));
				goto destroy_target_thread_affinity;
			}

			result = doca_dpa_eu_affinity_set(resources->affinities[i], threads_list[i]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Function doca_dpa_eu_affinity_set failed (%s)",
					     doca_error_get_descr(result));
				goto destroy_target_thread_affinity;
			}
		}
	}

	return result;

destroy_rdma_doca_dpa:
	if (resources->rdma_dpa_ctx != resources->pf_dpa_ctx)
		doca_dpa_destroy(resources->rdma_dpa_ctx);

destroy_target_thread_affinity:
	for (uint32_t i = 0; i < threads_num; ++i) {
		if (resources->affinities[i] != NULL) {
			doca_dpa_eu_affinity_destroy(resources->affinities[i]);
		}
	}
	free(resources->affinities);

destroy_doca_dpa:
	doca_dpa_destroy(resources->pf_dpa_ctx);
close_doca_dev:
	doca_dev_close(resources->pf_doca_device);
	if (resources->pf_doca_device != resources->rdma_doca_device)
		doca_dev_close(resources->rdma_doca_device);
exit_label:
	return result;
}

doca_error_t destroy_dpa_resources(struct dpa_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp_result;

	for (uint32_t i = 0; i < resources->num_affinities; ++i) {
		tmp_result = doca_dpa_eu_affinity_destroy(resources->affinities[i]);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_dpa_eu_affinity_destroy failed: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->affinities);

	if (resources->rdma_dpa_ctx != resources->pf_dpa_ctx) {
		tmp_result = doca_dpa_destroy(resources->rdma_dpa_ctx);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_dpa_destroy() failed: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	tmp_result = doca_dpa_destroy(resources->pf_dpa_ctx);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_destroy() failed: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	if (resources->pf_doca_device != resources->rdma_doca_device) {
		tmp_result = doca_dev_close(resources->rdma_doca_device);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	tmp_result = doca_dev_close(resources->pf_doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t dpa_thread_obj_init(struct dpa_thread_obj *dpa_thread_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS;

	doca_err = doca_dpa_thread_create(dpa_thread_obj->doca_dpa, &(dpa_thread_obj->thread));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_create failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	doca_err = doca_dpa_thread_set_func_arg(dpa_thread_obj->thread, dpa_thread_obj->func, dpa_thread_obj->arg);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_set_func_arg failed (%s)", doca_error_get_descr(doca_err));
		dpa_thread_obj_destroy(dpa_thread_obj);
		return doca_err;
	}

	if (dpa_thread_obj->tls_dev_ptr) {
		doca_err = doca_dpa_thread_set_local_storage(dpa_thread_obj->thread, dpa_thread_obj->tls_dev_ptr);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_dpa_thread_set_local_storage failed (%s)",
				     doca_error_get_descr(doca_err));
			dpa_thread_obj_destroy(dpa_thread_obj);
			return doca_err;
		}
	}

	if (dpa_thread_obj->affinity != NULL) {
		doca_err = doca_dpa_thread_set_affinity(dpa_thread_obj->thread, dpa_thread_obj->affinity);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_dpa_thread_set_affinity failed (%s)",
				     doca_error_get_descr(doca_err));
			dpa_thread_obj_destroy(dpa_thread_obj);
			return doca_err;
		}
	}

	doca_err = doca_dpa_thread_start(dpa_thread_obj->thread);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_start failed (%s)", doca_error_get_descr(doca_err));
		dpa_thread_obj_destroy(dpa_thread_obj);
		return doca_err;
	}

	return doca_err;
}

doca_error_t dpa_thread_obj_destroy(struct dpa_thread_obj *dpa_thread_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	doca_err = doca_dpa_thread_destroy(dpa_thread_obj->thread);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_thread_destroy failed (%s)", doca_error_get_descr(doca_err));
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	return ret_err;
}

doca_error_t dpa_completion_obj_init(struct dpa_completion_obj *dpa_completion_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS;

	doca_err = doca_dpa_completion_create(dpa_completion_obj->doca_dpa,
					      dpa_completion_obj->queue_size,
					      &(dpa_completion_obj->dpa_comp));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_completion_create failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	if (dpa_completion_obj->thread) {
		doca_err = doca_dpa_completion_set_thread(dpa_completion_obj->dpa_comp, dpa_completion_obj->thread);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_dpa_completion_set_thread failed (%s)",
				     doca_error_get_descr(doca_err));
			dpa_completion_obj_destroy(dpa_completion_obj);
			return doca_err;
		}
	}

	doca_err = doca_dpa_completion_start(dpa_completion_obj->dpa_comp);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_completion_start failed (%s)", doca_error_get_descr(doca_err));
		dpa_completion_obj_destroy(dpa_completion_obj);
		return doca_err;
	}

	doca_err = doca_dpa_completion_get_dpa_handle(dpa_completion_obj->dpa_comp, &(dpa_completion_obj->handle));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_completion_get_dpa_handle failed (%s)", doca_error_get_descr(doca_err));
		dpa_completion_obj_destroy(dpa_completion_obj);
		return doca_err;
	}

	return DOCA_SUCCESS;
}

doca_error_t dpa_completion_obj_destroy(struct dpa_completion_obj *dpa_completion_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	doca_err = doca_dpa_completion_destroy(dpa_completion_obj->dpa_comp);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_completion_destroy failed (%s)", doca_error_get_descr(doca_err));
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	return ret_err;
}

doca_error_t dpa_notification_completion_obj_init(struct dpa_notification_completion_obj *notification_completion_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS;

	doca_err = doca_dpa_notification_completion_create(notification_completion_obj->doca_dpa,
							   notification_completion_obj->thread,
							   &(notification_completion_obj->notification_comp));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_notification_completion_create failed (%s)",
			     doca_error_get_descr(doca_err));
		return doca_err;
	}

	doca_err = doca_dpa_notification_completion_start(notification_completion_obj->notification_comp);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_notification_completion_start failed (%s)",
			     doca_error_get_descr(doca_err));
		dpa_notification_completion_obj_destroy(notification_completion_obj);
		return doca_err;
	}

	doca_err = doca_dpa_notification_completion_get_dpa_handle(notification_completion_obj->notification_comp,
								   &(notification_completion_obj->handle));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_notification_completion_get_dpa_handle failed (%s)",
			     doca_error_get_descr(doca_err));
		dpa_notification_completion_obj_destroy(notification_completion_obj);
		return doca_err;
	}

	return DOCA_SUCCESS;
}

doca_error_t dpa_notification_completion_obj_destroy(struct dpa_notification_completion_obj *notification_completion_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	doca_err = doca_dpa_notification_completion_destroy(notification_completion_obj->notification_comp);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_dpa_notification_completion_destroy failed (%s)",
			     doca_error_get_descr(doca_err));
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	return ret_err;
}

doca_error_t dpa_rdma_obj_init(struct dpa_rdma_obj *dpa_rdma_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS;

	doca_err = doca_rdma_create(dpa_rdma_obj->doca_device, &(dpa_rdma_obj->rdma));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("RDMA create failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	dpa_rdma_obj->rdma_as_ctx = doca_rdma_as_ctx(dpa_rdma_obj->rdma);

	doca_err = doca_rdma_set_permissions(dpa_rdma_obj->rdma, dpa_rdma_obj->permissions);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_set_permissions failed (%s)", doca_error_get_descr(doca_err));
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	doca_err = doca_rdma_set_grh_enabled(dpa_rdma_obj->rdma, true);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_set_grh_enabled failed (%s)", doca_error_get_descr(doca_err));
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	doca_err = doca_ctx_set_datapath_on_dpa(dpa_rdma_obj->rdma_as_ctx, dpa_rdma_obj->doca_dpa);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_ctx_set_datapath_on_dpa failed (%s)", doca_error_get_descr(doca_err));
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	if (dpa_rdma_obj->recv_queue_size) {
		doca_err = doca_rdma_set_recv_queue_size(dpa_rdma_obj->rdma, dpa_rdma_obj->recv_queue_size);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_rdma_set_recv_queue_size failed (%s)\n",
				     doca_error_get_descr(doca_err));
			dpa_rdma_obj_destroy(dpa_rdma_obj);
			return doca_err;
		}
	}

	if (dpa_rdma_obj->buf_list_len) {
		doca_err = doca_rdma_task_receive_set_dst_buf_list_len(dpa_rdma_obj->rdma, dpa_rdma_obj->buf_list_len);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_rdma_task_receive_set_dst_buf_list_len failed (%s)\n",
				     doca_error_get_descr(doca_err));
			dpa_rdma_obj_destroy(dpa_rdma_obj);
			return doca_err;
		}
	}

	if (dpa_rdma_obj->is_gid_index_set) {
		doca_err = doca_rdma_set_gid_index(dpa_rdma_obj->rdma, dpa_rdma_obj->gid_index);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_rdma_set_gid_index failed (%s)\n", doca_error_get_descr(doca_err));
			dpa_rdma_obj_destroy(dpa_rdma_obj);
			return doca_err;
		}
	}

	if (dpa_rdma_obj->max_connections_count) {
		doca_err = doca_rdma_set_max_num_connections(dpa_rdma_obj->rdma, dpa_rdma_obj->max_connections_count);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_rdma_set_max_num_connections failed (%s)\n",
				     doca_error_get_descr(doca_err));
			dpa_rdma_obj_destroy(dpa_rdma_obj);
			return doca_err;
		}
	}

	doca_err = doca_rdma_dpa_completion_attach(dpa_rdma_obj->rdma, dpa_rdma_obj->dpa_comp);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_dpa_completion_attach failed (%s)", doca_error_get_descr(doca_err));
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	return doca_err;
}

doca_error_t dpa_rdma_obj_start(struct dpa_rdma_obj *dpa_rdma_obj)
{
	doca_error_t doca_err = doca_ctx_start(dpa_rdma_obj->rdma_as_ctx);
	if (doca_err != DOCA_SUCCESS) {
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	doca_err = doca_rdma_get_dpa_handle(dpa_rdma_obj->rdma, &(dpa_rdma_obj->dpa_rdma));
	if (doca_err != DOCA_SUCCESS) {
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	doca_err = doca_rdma_export(dpa_rdma_obj->rdma,
				    &(dpa_rdma_obj->connection_details),
				    &(dpa_rdma_obj->conn_det_len),
				    &(dpa_rdma_obj->connection));
	if (doca_err != DOCA_SUCCESS) {
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	doca_err = doca_rdma_connection_get_id(dpa_rdma_obj->connection, &(dpa_rdma_obj->connection_id));
	if (doca_err != DOCA_SUCCESS) {
		dpa_rdma_obj_destroy(dpa_rdma_obj);
		return doca_err;
	}

	if (dpa_rdma_obj->second_connection_needed) {
		doca_err = doca_rdma_export(dpa_rdma_obj->rdma,
					    &(dpa_rdma_obj->connection2_details),
					    &(dpa_rdma_obj->conn2_det_len),
					    &(dpa_rdma_obj->connection2));
		if (doca_err != DOCA_SUCCESS) {
			dpa_rdma_obj_destroy(dpa_rdma_obj);
			return doca_err;
		}

		doca_err = doca_rdma_connection_get_id(dpa_rdma_obj->connection2, &(dpa_rdma_obj->connection2_id));
		if (doca_err != DOCA_SUCCESS) {
			dpa_rdma_obj_destroy(dpa_rdma_obj);
			return doca_err;
		}
	}

	return doca_err;
}

doca_error_t dpa_rdma_obj_destroy(struct dpa_rdma_obj *dpa_rdma_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	doca_err = doca_ctx_stop(dpa_rdma_obj->rdma_as_ctx);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_ctx_stop failed (%s)", doca_error_get_descr(doca_err));
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	doca_err = doca_rdma_destroy(dpa_rdma_obj->rdma);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_rdma_destroy failed (%s)", doca_error_get_descr(doca_err));
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	return ret_err;
}

doca_error_t doca_mmap_obj_init(struct doca_mmap_obj *doca_mmap_obj)
{
	doca_error_t doca_err = doca_mmap_create(&(doca_mmap_obj->mmap));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_create failed (%s)", doca_error_get_descr(doca_err));
		return doca_err;
	}

	doca_err = doca_mmap_set_permissions(doca_mmap_obj->mmap, doca_mmap_obj->permissions);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_set_permissions failed (%s)", doca_error_get_descr(doca_err));
		doca_mmap_obj_destroy(doca_mmap_obj);
		return doca_err;
	}

	switch (doca_mmap_obj->mmap_type) {
	case MMAP_TYPE_CPU:
		doca_err = doca_mmap_set_memrange(doca_mmap_obj->mmap,
						  doca_mmap_obj->memrange_addr,
						  doca_mmap_obj->memrange_len);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_mmap_set_memrange failed (%s)", doca_error_get_descr(doca_err));
			doca_mmap_obj_destroy(doca_mmap_obj);
			return doca_err;
		}

		doca_err = doca_mmap_add_dev(doca_mmap_obj->mmap, doca_mmap_obj->doca_device);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_mmap_add_dev failed (%s)", doca_error_get_descr(doca_err));
			doca_mmap_obj_destroy(doca_mmap_obj);
			return doca_err;
		}

		break;

	case MMAP_TYPE_DPA:
		doca_err = doca_mmap_set_dpa_memrange(doca_mmap_obj->mmap,
						      doca_mmap_obj->doca_dpa,
						      (uint64_t)doca_mmap_obj->memrange_addr,
						      doca_mmap_obj->memrange_len);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Function doca_mmap_set_dpa_memrange failed (%s)", doca_error_get_descr(doca_err));
			doca_mmap_obj_destroy(doca_mmap_obj);
			return doca_err;
		}

		break;

	default:
		DOCA_LOG_ERR("Unsupported mmap_type (%d)", doca_mmap_obj->mmap_type);
		doca_mmap_obj_destroy(doca_mmap_obj);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	doca_err = doca_mmap_start(doca_mmap_obj->mmap);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_start failed (%s)", doca_error_get_descr(doca_err));
		doca_mmap_obj_destroy(doca_mmap_obj);
		return doca_err;
	}

	doca_err = doca_mmap_dev_get_dpa_handle(doca_mmap_obj->mmap,
						doca_mmap_obj->doca_device,
						&(doca_mmap_obj->dpa_mmap_handle));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_dev_get_dpa_handle failed (%s)", doca_error_get_descr(doca_err));
		doca_mmap_obj_destroy(doca_mmap_obj);
		return doca_err;
	}

	doca_err = doca_mmap_export_rdma(doca_mmap_obj->mmap,
					 doca_mmap_obj->doca_device,
					 &(doca_mmap_obj->rdma_export),
					 &(doca_mmap_obj->export_len));
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_export_rdma failed (%s)", doca_error_get_descr(doca_err));
		doca_mmap_obj_destroy(doca_mmap_obj);
		return doca_err;
	}

	return doca_err;
}

doca_error_t doca_mmap_obj_destroy(struct doca_mmap_obj *doca_mmap_obj)
{
	doca_error_t doca_err = DOCA_SUCCESS, ret_err = DOCA_SUCCESS;

	doca_err = doca_mmap_destroy(doca_mmap_obj->mmap);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Function doca_mmap_destroy failed (%s)", doca_error_get_descr(doca_err));
		DOCA_ERROR_PROPAGATE(ret_err, doca_err);
	}

	return ret_err;
}
