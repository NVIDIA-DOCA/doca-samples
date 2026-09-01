/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_mgmt.h>
#include <doca_mgmt_device_caps_general.h>

#include "common.h"

/*
 * Bundle of resources used to manage a VF/SF's data direct capability. Populated by mgmt_data_direct_ctx_create() and
 * released by mgmt_data_direct_ctx_destroy().
 */
struct mgmt_data_direct_ctx {
	struct doca_dev *dev;		       /* PF device (opened for the VF PCI address path only) */
	struct doca_dev_rep *dev_rep;	       /* VF representor (opened for the Ethernet VF PCI address path only) */
	struct doca_mgmt_dev_ctx *dev_ctx;     /* Device management context; NULL when owned by @rep_ctx */
	struct doca_mgmt_dev_rep_ctx *rep_ctx; /* Representor management context used for the data direct operations */
};

/*
 * Get the PF PCI address of the given VF over sysfs
 *
 * @vf_pci_addr [in]: The PCI address of the VF
 * @pf_pci_addr [out]: Buffer to store the PF PCI address. Must be at least DOCA_DEVINFO_PCI_ADDR_SIZE bytes long.
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_pf_pci_addr(const char *vf_pci_addr, char *pf_pci_addr)
{
	char physfn_path[PATH_MAX];
	char physfn_link[PATH_MAX];
	char *pf_pci_addr_start;
	ssize_t rc;
	int ret;
	doca_error_t result;

	ret = snprintf(physfn_path, PATH_MAX, "/sys/bus/pci/devices/%s/physfn", vf_pci_addr);
	if (ret < 0 || ret >= PATH_MAX) {
		fprintf(stderr, "Failed to get PF PCI address: failed to create physfn path\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	rc = readlink(physfn_path, physfn_link, PATH_MAX);
	if (rc < 0 || rc == PATH_MAX) {
		if (rc == PATH_MAX) {
			ret = EINVAL;
			result = DOCA_ERROR_INVALID_VALUE;
		} else {
			ret = errno;
			result = DOCA_ERROR_OPERATING_SYSTEM;
		}
		fprintf(stderr,
			"Failed to get PF PCI address: failed to read physfn link, errno: %s (%d)\n",
			strerror(ret),
			ret);
		return result;
	}
	physfn_link[rc] = '\0';

	pf_pci_addr_start = strrchr(physfn_link, '/');
	if (pf_pci_addr_start == NULL) {
		fprintf(stderr, "Failed to get PF PCI address: failed to parse physfn link %s\n", physfn_link);
		return DOCA_ERROR_INVALID_VALUE;
	}
	pf_pci_addr_start++;
	strncpy(pf_pci_addr, pf_pci_addr_start, DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
	pf_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE - 1] = '\0';

	return DOCA_SUCCESS;
}

/*
 * Check if the PF's IB device link layer is InfiniBand
 *
 * @pf_pci_addr [in]: The PCI address of the PF
 * @is_ib_link_layer [out]: Flag to indicate if the PF's IB device link layer is InfiniBand
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pf_is_ib_link_layer(const char *pf_pci_addr, bool *is_ib_link_layer)
{
	char infiniband_path[PATH_MAX];
	char link_layer_path[PATH_MAX];
	char link_layer_value[64];
	DIR *dir;
	struct dirent *entry;
	FILE *fp = NULL;
	size_t len;
	doca_error_t result;
	int ret;

	ret = snprintf(infiniband_path, PATH_MAX, "/sys/bus/pci/devices/%s/infiniband", pf_pci_addr);
	if (ret < 0 || ret >= PATH_MAX) {
		fprintf(stderr, "Failed to check PF IB link layer: failed to create infiniband path\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	dir = opendir(infiniband_path);
	if (dir == NULL) {
		fprintf(stderr,
			"Failed to check PF IB link layer: failed to open %s directory. errno: %s (%d)\n",
			infiniband_path,
			strerror(errno),
			errno);
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;

		ret = snprintf(link_layer_path, PATH_MAX, "%s/%s/ports/1/link_layer", infiniband_path, entry->d_name);
		if (ret < 0 || ret >= PATH_MAX) {
			fprintf(stderr,
				"Failed to check PF IB link layer: failed to create link_layer path for IB device %s\n",
				entry->d_name);
			result = DOCA_ERROR_INVALID_VALUE;
			goto out;
		}

		fp = fopen(link_layer_path, "r");
		if (fp == NULL) {
			fprintf(stderr,
				"Failed to check PF IB link layer: failed to open file %s. errno: %s (%d)\n",
				link_layer_path,
				strerror(errno),
				errno);
			result = DOCA_ERROR_OPERATING_SYSTEM;
			goto out;
		}

		if (fgets(link_layer_value, sizeof(link_layer_value), fp) == NULL) {
			fprintf(stderr,
				"Failed to check PF IB link layer: failed to read %s file. errno: %s (%d)\n",
				link_layer_path,
				strerror(errno),
				errno);
			result = DOCA_ERROR_OPERATING_SYSTEM;
			goto out_close_fp;
		}

		/* Remove trailing newline if present */
		len = strlen(link_layer_value);
		if (len > 0 && link_layer_value[len - 1] == '\n')
			link_layer_value[len - 1] = '\0';

		*is_ib_link_layer = strcmp(link_layer_value, "InfiniBand") == 0;
		result = DOCA_SUCCESS;
		goto out_close_fp;
	}

	fprintf(stderr, "Failed to check PF IB link layer: failed to find IB device\n");
	result = DOCA_ERROR_INVALID_VALUE;

out_close_fp:
	if (fp != NULL)
		(void)fclose(fp);

out:
	(void)closedir(dir);
	return result;
}

/*
 * Release all resources held by @dd_ctx. Safe to call on a partially-initialized @dd_ctx (NULL fields are skipped).
 *
 * @dd_ctx [in]: The management resources to release
 */
static void mgmt_data_direct_ctx_destroy(struct mgmt_data_direct_ctx *dd_ctx)
{
	if (dd_ctx->rep_ctx != NULL && doca_mgmt_dev_rep_ctx_destroy(dd_ctx->rep_ctx) != DOCA_SUCCESS)
		fprintf(stderr, "Failed to destroy DOCA management device representor context\n");

	/* dev_ctx is set only when it is not owned by the representor context */
	if (dd_ctx->dev_ctx != NULL && doca_mgmt_dev_ctx_destroy(dd_ctx->dev_ctx) != DOCA_SUCCESS)
		fprintf(stderr, "Failed to destroy DOCA management device context\n");

	if (dd_ctx->dev_rep != NULL && doca_dev_rep_close(dd_ctx->dev_rep) != DOCA_SUCCESS)
		fprintf(stderr, "Failed to close DOCA device representor\n");

	if (dd_ctx->dev != NULL && doca_dev_close(dd_ctx->dev) != DOCA_SUCCESS)
		fprintf(stderr, "Failed to close DOCA device\n");
}

/*
 * Create the representor management context (and any underlying resources) for the target VF/SF.
 *
 * The representor can be selected in two ways:
 * - If @rep_target_string is not NULL, it is resolved via doca_mgmt_dev_rep_ctx_create_from_target_string().  The
 *   returned representor context owns its parent device context.
 * - Otherwise, @vf_pci_addr identifies the VF: its PF device is opened, and depending on the PF's link layer either the
 *   VF representor is opened (Ethernet) or the representor context is created directly by PCI address (InfiniBand).
 *
 * On success @dd_ctx holds the created resources and must be released with mgmt_data_direct_ctx_destroy().  On failure
 * nothing is left open.
 *
 * @rep_target_string [in]: Representor management target string, or NULL to use @vf_pci_addr
 * @vf_pci_addr [in]: The VF PCI address (used only when @rep_target_string is NULL)
 * @dd_ctx [out]: Populated with the created resources on success
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mgmt_data_direct_ctx_create(const char *rep_target_string,
						const char *vf_pci_addr,
						struct mgmt_data_direct_ctx *dd_ctx)
{
	char pf_pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE];
	bool is_ib;
	doca_error_t result;

	memset(dd_ctx, 0, sizeof(*dd_ctx));

	if (rep_target_string != NULL) {
		/* The created representor context owns its parent device context */
		result = doca_mgmt_dev_rep_ctx_create_from_target_string(rep_target_string, &dd_ctx->rep_ctx);
		if (result != DOCA_SUCCESS)
			fprintf(stderr,
				"Failed to create DOCA management device representor context: %s\n",
				doca_error_get_descr(result));

		return result;
	}

	/* Get the PF PCI address of the VF and open the PF device */
	result = get_pf_pci_addr(vf_pci_addr, pf_pci_addr);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr,
			"Failed to get PF PCI address of VF %s: %s\n",
			vf_pci_addr,
			doca_error_get_descr(result));
		return result;
	}

	result = open_doca_device_with_pci(pf_pci_addr, NULL, &dd_ctx->dev);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to open DOCA device of PF %s: %s\n", pf_pci_addr, doca_error_get_descr(result));
		return result;
	}

	result = pf_is_ib_link_layer(pf_pci_addr, &is_ib);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr,
			"Failed to check IB link layer of PF %s: %s\n",
			pf_pci_addr,
			doca_error_get_descr(result));
		goto err;
	}

	/*
	 * If the PF's IB device link layer is Ethernet, the VF is expected to have a representor, so open it.
	 * Otherwise (the link layer is Infiniband), the VF doesn't have a representor and we will create the
	 * representor context directly from the VF PCI address.
	 */
	if (!is_ib) {
		result = open_doca_device_rep_with_pci(dd_ctx->dev,
						       DOCA_DEVINFO_REP_FILTER_NET,
						       vf_pci_addr,
						       &dd_ctx->dev_rep);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr,
				"Failed to open DOCA device representor of VF %s: %s\n",
				vf_pci_addr,
				doca_error_get_descr(result));
			goto err;
		}
	}

	/* Create the DOCA management device context for the PF */
	result = doca_mgmt_dev_ctx_create(dd_ctx->dev, &dd_ctx->dev_ctx);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create DOCA management device context: %s\n", doca_error_get_descr(result));
		goto err;
	}

	if (dd_ctx->dev_rep != NULL) {
		/* Create the DOCA management device representor context from the VF representor */
		result = doca_mgmt_dev_rep_ctx_create(dd_ctx->dev_ctx, dd_ctx->dev_rep, &dd_ctx->rep_ctx);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr,
				"Failed to create DOCA management device representor context: %s\n",
				doca_error_get_descr(result));
			goto err;
		}
	} else {
		/* Create the DOCA management device representor context by PCI address (VF without a representor) */
		result = doca_mgmt_dev_rep_ctx_create_by_pci_addr(dd_ctx->dev_ctx, vf_pci_addr, &dd_ctx->rep_ctx);
		if (result != DOCA_SUCCESS) {
			fprintf(stderr,
				"Failed to create DOCA management device representor context by PCI address %s: %s\n",
				vf_pci_addr,
				doca_error_get_descr(result));
			goto err;
		}
	}

	return DOCA_SUCCESS;

err:
	mgmt_data_direct_ctx_destroy(dd_ctx);
	return result;
}

/*
 * Get the data direct attribute for a device representor.
 *
 * @rep_target_string [in]: Representor management target string, or NULL to use @vf_pci_addr
 * @vf_pci_addr [in]: The PCI address of the VF (used only when @rep_target_string is NULL)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_data_direct_get(const char *rep_target_string, const char *vf_pci_addr)
{
	struct mgmt_data_direct_ctx dd_ctx;
	struct doca_mgmt_device_caps_general *device_caps_general;
	uint8_t data_direct;
	doca_error_t result;

	result = mgmt_data_direct_ctx_create(rep_target_string, vf_pci_addr, &dd_ctx);
	if (result != DOCA_SUCCESS)
		return result;

	/* Create the device caps general handle */
	result = doca_mgmt_device_caps_general_create(&device_caps_general);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create device caps general handle: %s\n", doca_error_get_descr(result));
		goto out_ctx_destroy;
	}

	/* Execute the get command */
	result = doca_mgmt_device_caps_general_get(dd_ctx.rep_ctx, device_caps_general);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to get device caps general configuration: %s\n", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

	/* Get the data direct attribute */
	result = doca_mgmt_device_caps_general_get_data_direct(device_caps_general, &data_direct);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to get data direct attribute: %s\n", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

	printf("Data direct: %s\n", data_direct ? "ENABLED" : "DISABLED");

out_device_caps_general_destroy:
	if (doca_mgmt_device_caps_general_destroy(device_caps_general) != DOCA_SUCCESS)
		fprintf(stderr, "Failed to destroy device caps general handle\n");

out_ctx_destroy:
	mgmt_data_direct_ctx_destroy(&dd_ctx);

	return result;
}

/*
 * Set the data direct attribute for a device representor.
 *
 * @rep_target_string [in]: Representor management target string, or NULL to use @vf_pci_addr
 * @vf_pci_addr [in]: The PCI address of the VF (used only when @rep_target_string is NULL)
 * @enabled [in]: Flag to indicate if data direct should be enabled or disabled
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t mgmt_data_direct_set(const char *rep_target_string, const char *vf_pci_addr, bool enabled)
{
	struct mgmt_data_direct_ctx dd_ctx;
	struct doca_mgmt_device_caps_general *device_caps_general;
	doca_error_t result;

	result = mgmt_data_direct_ctx_create(rep_target_string, vf_pci_addr, &dd_ctx);
	if (result != DOCA_SUCCESS)
		return result;

	/* Create the device caps general handle */
	result = doca_mgmt_device_caps_general_create(&device_caps_general);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create device caps general handle: %s\n", doca_error_get_descr(result));
		goto out_ctx_destroy;
	}

	/* Set the attributes */
	result = doca_mgmt_device_caps_general_set_data_direct(device_caps_general, enabled);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set data direct attribute: %s\n", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

	/* Execute the set command */
	result = doca_mgmt_device_caps_general_set(dd_ctx.rep_ctx, device_caps_general);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to set device caps general configuration: %s\n", doca_error_get_descr(result));
		goto out_device_caps_general_destroy;
	}

out_device_caps_general_destroy:
	if (doca_mgmt_device_caps_general_destroy(device_caps_general) != DOCA_SUCCESS)
		fprintf(stderr, "Failed to destroy device caps general handle\n");

out_ctx_destroy:
	mgmt_data_direct_ctx_destroy(&dd_ctx);

	return result;
}
