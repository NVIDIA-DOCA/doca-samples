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

#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include <doca_dev.h>

#include <doca_ctx.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_type.h>
#include <doca_error.h>
#include <doca_log.h>

#include "nvme_pci_common.h"
#include "nvme_pci_type_config.h"

/*
 * List all DOCA devices that are emulation manager
 *
 * Implements the nvmf_doca_get_managers SPDK RPC call
 *
 * @request [in]: The RPC request json object
 * @params [in]: The RPC parameters json object
 */
static void rpc_nvmf_doca_get_managers(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	(void)params;

	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	char name[DOCA_DEVINFO_IBDEV_NAME_SIZE];
	uint8_t is_hotplug_manager;
	doca_error_t ret;
	struct spdk_json_write_ctx *w;

	ret = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "doca_devinfo_create_list failed\n");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	for (uint32_t idx = 0; idx < nb_devs; idx++) {
		ret = doca_devinfo_cap_is_hotplug_manager_supported(dev_list[idx], &is_hotplug_manager);
		if (ret == DOCA_SUCCESS && is_hotplug_manager) {
			ret = doca_devinfo_get_ibdev_name(dev_list[idx], name, 64);
			if (ret == DOCA_SUCCESS) {
				spdk_json_write_object_begin(w);
				spdk_json_write_named_string(w, "name", name);
				spdk_json_write_object_end(w);
			} else
				SPDK_ERRLOG("failed to get ibdev name %d\n", ret);
		}
	}
	spdk_json_write_array_end(w);
	doca_devinfo_destroy_list(dev_list);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("nvmf_doca_get_managers", rpc_nvmf_doca_get_managers, SPDK_RPC_RUNTIME);

/**
 * params of create function RPC
 */
struct nvmf_doca_create_function_in {
	char *dev_name; /**< Device name.*/
};

static void free_rpc_nvmf_doca_create_function(struct nvmf_doca_create_function_in *in)
{
	free(in->dev_name);
}

static const struct spdk_json_object_decoder nvmf_doca_create_function_decoder[] = {
	{"dev-name", offsetof(struct nvmf_doca_create_function_in, dev_name), spdk_json_decode_string, true},
};

/*
 * Creates an emulated function
 *
 * Implements the nvmf_doca_create_function SPDK RPC call
 *
 * @request [in]: The RPC request json object
 * @params [in]: The RPC parameters json object
 */
static void rpc_nvmf_doca_create_function(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct nvmf_doca_create_function_in attr = {0};
	struct doca_dev_rep *dev_rep;
	char buf[DOCA_DEVINFO_REP_VUID_SIZE];
	struct spdk_json_write_ctx *w;
	struct doca_devemu_pci_type *pci_type = {0};
	struct doca_dev *dev = {0};
	doca_error_t ret;

	if (params == NULL || spdk_json_decode_object(params,
						      nvmf_doca_create_function_decoder,
						      SPDK_COUNTOF(nvmf_doca_create_function_decoder),
						      &attr)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto cleanup;
	}

	ret = create_find_start_pci_type(attr.dev_name, &pci_type, &dev);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Couldn't find supported devices");
		goto cleanup;
	}

	ret = doca_devemu_pci_type_create_rep(pci_type, &dev_rep);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Couldn't create the device representor\n");
		goto destroy_pci_resources;
	}

	ret = doca_devinfo_rep_get_vuid(doca_dev_rep_as_devinfo(dev_rep), buf, DOCA_DEVINFO_REP_VUID_SIZE);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Couldnt get device VUID");
		goto destroy_dev_rep;
	}

	ret = doca_dev_rep_close(dev_rep);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to cloes the device");
		goto destroy_dev_rep;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "Created a function with vuid", buf);
	spdk_json_write_object_end(w);

	spdk_jsonrpc_end_result(request, w);

	goto destroy_pci_resources;

destroy_dev_rep:
	doca_devemu_pci_type_destroy_rep(dev_rep);
destroy_pci_resources:
	cleanup_pci_resources(pci_type, dev);
cleanup:
	free_rpc_nvmf_doca_create_function(&attr);
}
SPDK_RPC_REGISTER("nvmf_doca_create_function", rpc_nvmf_doca_create_function, SPDK_RPC_RUNTIME);

/**
 * params of destroy function RPC
 */
struct nvmf_doca_destroy_function_in {
	char *dev_name; /**< Device name.*/
	char *vuid;	/**< vuid.*/
};

static void free_rpc_nvmf_doca_destroy_function(struct nvmf_doca_destroy_function_in *in)
{
	free(in->dev_name);
	free(in->vuid);
}

static const struct spdk_json_object_decoder nvmf_doca_destroy_function_decoder[] = {
	{"dev-name", offsetof(struct nvmf_doca_destroy_function_in, dev_name), spdk_json_decode_string, true},
	{"vuid", offsetof(struct nvmf_doca_destroy_function_in, vuid), spdk_json_decode_string, true},
};

/*
 * Destroys an emulated function
 *
 * Implements the nvmf_doca_destroy_function SPDK RPC call
 *
 * @request [in]: The RPC request json object
 * @params [in]: The RPC parameters json object
 */
static void rpc_nvmf_doca_destroy_function(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct nvmf_doca_destroy_function_in attr = {0};
	struct doca_devinfo_rep **devinfo_rep_list;
	uint32_t nb_devs_reps;
	struct doca_dev_rep *dev_rep;
	char buf[DOCA_DEVINFO_REP_VUID_SIZE];
	doca_error_t ret;
	struct doca_devemu_pci_type *pci_type = {0};
	struct doca_dev *dev = {0};

	if (params == NULL || spdk_json_decode_object(params,
						      nvmf_doca_destroy_function_decoder,
						      SPDK_COUNTOF(nvmf_doca_destroy_function_decoder),
						      &attr)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto cleanup;
	}

	ret = create_find_start_pci_type(attr.dev_name, &pci_type, &dev);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Couldn't find supported devices");
		goto cleanup;
	}

	ret = doca_devemu_pci_type_create_rep_list(pci_type, &devinfo_rep_list, &nb_devs_reps);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Couldn't create the device representors list\n");
		cleanup_pci_resources(pci_type, dev);
		goto cleanup;
	}

	for (uint32_t i = 0; i < nb_devs_reps; i++) {
		ret = doca_devinfo_rep_get_vuid(devinfo_rep_list[i], buf, DOCA_DEVINFO_REP_VUID_SIZE);
		if (ret == DOCA_SUCCESS && (strncmp(buf, attr.vuid, DOCA_DEVINFO_REP_VUID_SIZE) == 0)) {
			ret = doca_dev_rep_open(devinfo_rep_list[i], &dev_rep);
			if (ret != DOCA_SUCCESS) {
				spdk_jsonrpc_send_error_response(request,
								 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
								 "Couldn't open the device representor");
				cleanup_pci_resources(pci_type, dev);
			}

			ret = doca_devemu_pci_type_destroy_rep(dev_rep);
			if (ret != DOCA_SUCCESS) {
				spdk_jsonrpc_send_error_response(request,
								 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
								 "Couldn't destroy device representor");
				doca_dev_rep_close(dev_rep);
				cleanup_pci_resources(pci_type, dev);
			}
			doca_devinfo_rep_destroy_list(devinfo_rep_list);
			cleanup_pci_resources(pci_type, dev);
			spdk_jsonrpc_send_bool_response(request, true);
			goto cleanup;
		}
	}

	doca_devinfo_rep_destroy_list(devinfo_rep_list);
	cleanup_pci_resources(pci_type, dev);
	spdk_jsonrpc_send_error_response(request,
					 SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "device representor with requested vuid doesn't exist");

cleanup:
	free_rpc_nvmf_doca_destroy_function(&attr);
}
SPDK_RPC_REGISTER("nvmf_doca_destroy_function", rpc_nvmf_doca_destroy_function, SPDK_RPC_RUNTIME);

/**
 * params of list functions RPC
 */
struct nvmf_doca_list_functions_in {
	char *dev_name; /**< Device name.*/
};

static void free_rpc_nvmf_doca_list_functions(struct nvmf_doca_list_functions_in *in)
{
	free(in->dev_name);
}

static const struct spdk_json_object_decoder nvmf_doca_list_functions_decoder[] = {
	{"dev-name", offsetof(struct nvmf_doca_list_functions_in, dev_name), spdk_json_decode_string, true},
};

/*
 * Lists the emulated functions
 *
 * Implements the nvmf_doca_list_functions SPDK RPC call
 *
 * @request [in]: The RPC request json object
 * @params [in]: The RPC parameters json object
 */
static void rpc_nvmf_doca_list_functions(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct nvmf_doca_list_functions_in attr = {0};
	struct doca_devinfo_rep **devinfo_rep_list;
	struct spdk_json_write_ctx *w;
	uint32_t nb_devs_reps;
	struct doca_devemu_pci_type *pci_type = {0};
	struct doca_dev *dev = {0};
	doca_error_t ret;
	char buf[DOCA_DEVINFO_REP_VUID_SIZE];

	if (params == NULL || spdk_json_decode_object(params,
						      nvmf_doca_list_functions_decoder,
						      SPDK_COUNTOF(nvmf_doca_list_functions_decoder),
						      &attr)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto cleanup;
	}

	ret = create_find_start_pci_type(attr.dev_name, &pci_type, &dev);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Couldn't find supported devices");
		goto cleanup;
	}

	ret = doca_devemu_pci_type_create_rep_list(pci_type, &devinfo_rep_list, &nb_devs_reps);
	if (ret != DOCA_SUCCESS) {
		spdk_jsonrpc_send_error_response(request,
						 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Couldn't create the device representors list\n");
		cleanup_pci_resources(pci_type, dev);
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	for (uint32_t i = 0; i < nb_devs_reps; i++) {
		char rep_pci[DOCA_DEVINFO_REP_PCI_ADDR_SIZE];

		spdk_json_write_object_begin(w);

		ret = doca_devinfo_rep_get_vuid(devinfo_rep_list[i], buf, DOCA_DEVINFO_REP_VUID_SIZE);
		if (ret != DOCA_SUCCESS) {
			spdk_jsonrpc_send_error_response(request,
							 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Couldnt get device VUID");
			doca_devinfo_rep_destroy_list(devinfo_rep_list);
			cleanup_pci_resources(pci_type, dev);
			goto cleanup;
		}
		ret = doca_devinfo_rep_get_pci_addr_str(devinfo_rep_list[i], rep_pci);
		if (ret != DOCA_SUCCESS) {
			spdk_jsonrpc_send_error_response(request,
							 SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Couldnt get PCI address");
			doca_devinfo_rep_destroy_list(devinfo_rep_list);
			cleanup_pci_resources(pci_type, dev);
			goto cleanup;
		}

		spdk_json_write_named_string(w, "Function VUID: ", buf);
		spdk_json_write_named_string(w, "PCI Address: ", rep_pci);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
	doca_devinfo_rep_destroy_list(devinfo_rep_list);
	cleanup_pci_resources(pci_type, dev);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_nvmf_doca_list_functions(&attr);
}
SPDK_RPC_REGISTER("nvmf_doca_list_functions", rpc_nvmf_doca_list_functions, SPDK_RPC_RUNTIME);