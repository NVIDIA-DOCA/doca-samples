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

#include <doca_log.h>
#include <doca_dev.h>
#include <doca_devemu_vfs_type.h>

#include <virtiofs_core.h>
#include <virtiofs_manager.h>

DOCA_LOG_REGISTER(VIRTIOFS_MANAGER)

static doca_error_t virtiofs_function_destroy(struct virtiofs_function *func, bool rep_destroy)
{
	doca_error_t result;

	result = rep_destroy ? doca_devemu_pci_type_destroy_rep(func->rep) : doca_dev_rep_close(func->rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close doca_dev_rep, err: %s", doca_error_get_name(result));
		return result;
	}

	free(func);
	return DOCA_SUCCESS;
}

static doca_error_t virtiofs_function_list_destroy(struct virtiofs_manager *manager)
{
	struct virtiofs_function *func, *tmp;
	doca_error_t err;

	STAILQ_FOREACH_SAFE(func, &manager->funcs, entry, tmp)
	{
		err = virtiofs_function_destroy(func, false);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy virtiofs_function, err: %s", doca_error_get_name(err));
			return err;
		}
	}

	STAILQ_INIT(&manager->funcs);

	return DOCA_SUCCESS;
}

static struct virtiofs_function *virtiofs_function_create(struct doca_devinfo_rep *devinfo_rep)
{
	struct virtiofs_function *func;
	doca_error_t err;

	func = calloc(1, sizeof(struct virtiofs_function));
	if (!func) {
		DOCA_LOG_ERR("Failed to allocate memory for virtiofs_function");
		return NULL;
	}

	err = doca_devinfo_rep_get_is_hotplug(devinfo_rep, &func->is_hotplugged);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get hotplug status, ret: %d\n", err);
		return NULL;
	}

	err = doca_devinfo_rep_get_pci_addr_str(devinfo_rep, func->pci_buf);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get pci_addr, ret: %d\n", err);
		return NULL;
	}

	err = doca_devinfo_rep_get_vuid(devinfo_rep, func->vuid, DOCA_DEVINFO_REP_VUID_SIZE);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get vuid, ret: %d\n", err);
		return NULL;
	}

	err = doca_devinfo_rep_get_pci_func_type(devinfo_rep, &func->pci_function_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get pci function type, ret: %d\n", err);
		return NULL;
	}

	err = doca_dev_rep_open(devinfo_rep, &func->rep);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open doca_dev_rep, ret: %d\n", err);
		return NULL;
	}

	DOCA_LOG_DBG("VFS Function available, vuid: %s, pci_addr: %s, is_hotplugged: %d",
		     func->vuid,
		     func->pci_buf,
		     func->is_hotplugged);

	return func;
}

static doca_error_t virtiofs_function_list_create(struct virtiofs_manager *manager)
{
	struct doca_devinfo_rep **dev_list_rep = NULL;
	struct virtiofs_function *func;
	doca_error_t err = DOCA_SUCCESS;
	uint32_t num_funcs, idx;

	STAILQ_INIT(&manager->funcs);

	/* Query and get the rep(s) for this doca_dev */
	err = doca_devemu_pci_type_create_rep_list(doca_devemu_vfs_type_as_pci_type(manager->vfs_type),
						   &dev_list_rep,
						   &num_funcs);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get rep list, ret: %s", doca_error_get_descr(err));
		return err;
	}

	for (idx = 0; idx < num_funcs; idx++) {
		func = virtiofs_function_create(dev_list_rep[idx]);
		if (func == NULL) {
			DOCA_LOG_ERR("Failed to create virtiofs_function %u for manager: %s", idx, manager->name);
			virtiofs_function_list_destroy(manager);
			doca_devinfo_rep_destroy_list(dev_list_rep);
			return DOCA_ERROR_UNKNOWN;
		}

		STAILQ_INSERT_TAIL(&manager->funcs, func, entry);
	}

	doca_devinfo_rep_destroy_list(dev_list_rep);
	return DOCA_SUCCESS;
}

static struct virtiofs_manager *virtiofs_manager_create(struct doca_devinfo *devinfo)
{
	struct virtiofs_manager *manager;
	doca_error_t result;

	manager = calloc(1, sizeof(*manager));
	if (!manager) {
		return NULL;
	}

	result = doca_devinfo_get_ibdev_name(devinfo, manager->name, VIRTIOFS_MANAGER_NAME_LENGTH);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get ibdev name, err: %s", doca_error_get_name(result));
		goto out_free;
	}

	result = doca_dev_open(devinfo, &manager->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open doca_dev, err: %s", doca_error_get_name(result));
		goto out_free;
	}

	/* Get the vfs_type for the doca_dev */
	result = doca_devemu_vfs_find_default_vfs_type_by_dev(manager->dev, &manager->vfs_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to find vfs_type for doca_dev, ret: %s", doca_error_get_name(result));
		goto destroy_dev;
	}

	result = doca_devemu_pci_cap_type_is_hotplug_supported(devinfo,
							       doca_devemu_vfs_type_as_pci_type(manager->vfs_type),
							       &manager->hotplug_supported);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to check hotplug support, ret: %s", doca_error_get_name(result));
		goto destroy_dev;
	}

	result = virtiofs_function_list_create(manager);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtiofs_functions, ret: %s", doca_error_get_name(result));
		goto destroy_dev;
	}

	return manager;

destroy_dev:
	doca_dev_close(manager->dev);
out_free:
	free(manager);
	return NULL;
}

static void virtiofs_manager_destroy(struct virtiofs_manager *manager)
{
	doca_error_t result;

	result = virtiofs_function_list_destroy(manager);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy virtiofs_function list, err: %s", doca_error_get_name(result));

	result = doca_dev_close(manager->dev);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to close doca_dev, err: %s", doca_error_get_name(result));

	free(manager);
}

void virtiofs_managers_destroy(struct virtiofs_resources *ctx)
{
	struct virtiofs_manager *manager, *tmp;

	SLIST_FOREACH_SAFE(manager, &ctx->managers, entry, tmp)
	virtiofs_manager_destroy(manager);
}

doca_error_t virtiofs_managers_create(struct virtiofs_resources *ctx)
{
	doca_error_t result;
	uint8_t is_supported = false;
	uint32_t idx, num_devices = 0;
	struct doca_devinfo **devinfo_list;
	struct virtiofs_manager *manager;

	SLIST_INIT(&ctx->managers);

	result = doca_devinfo_create_list(&devinfo_list, &num_devices);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca devinfo list, err: %s", doca_error_get_name(result));
		return DOCA_ERROR_NO_MEMORY;
	}

	for (idx = 0; idx < num_devices; idx++) {
		result = doca_devemu_vfs_is_default_vfs_type_supported(devinfo_list[idx], &is_supported);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query Virtio FS support for doca_devinfo, err: %s",
				     doca_error_get_name(result));
			continue;
		}

		if (is_supported == false) {
			continue;
		}

		manager = virtiofs_manager_create(devinfo_list[idx]);
		if (manager) {
			SLIST_INSERT_HEAD(&ctx->managers, manager, entry);
			ctx->num_managers++;
		}
	}

	doca_devinfo_destroy_list(devinfo_list);

	if (SLIST_EMPTY(&ctx->managers)) {
		DOCA_LOG_ERR("Virtio FS supported doca manager not found");
		return DOCA_ERROR_NOT_FOUND;
	}

	return DOCA_SUCCESS;
}

struct virtiofs_manager *virtiofs_manager_get_by_name(struct virtiofs_resources *ctx, char *name)
{
	struct virtiofs_manager *manager;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	doca_error_t result;

	SLIST_FOREACH(manager, &ctx->managers, entry)
	{
		result = doca_devinfo_get_ibdev_name(doca_dev_as_devinfo(manager->dev), ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS)
			continue;

		if (!strncmp(name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE))
			return manager;
	}

	return NULL;
}

doca_error_t virtiofs_function_get_available(struct virtiofs_resources *ctx, struct virtiofs_function **func_out)
{
	struct virtiofs_manager *manager;
	struct virtiofs_function *func;

	SLIST_FOREACH(manager, &ctx->managers, entry)
	{
		STAILQ_FOREACH(func, &manager->funcs, entry)
		{
			if (func->is_hotplugged == false) {
				*func_out = func;
				return DOCA_SUCCESS;
			}
		}
	}

	return DOCA_ERROR_NOT_FOUND;
}
