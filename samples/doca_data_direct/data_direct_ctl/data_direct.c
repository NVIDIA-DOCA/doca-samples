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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "data_direct.h"
#include "data_direct_mlx5_ifc.h"
#include "mlxfwctl.h"
#include "vpd_parser.h"

#include "util.h"

#define VUID_VPD_KEYWORD "VU"
static char *dd_pci_device_get_vu(const char *bdf)
{
	enum vpd_parser_resource_tags vpd_tag;
	char vpd_path[DEV_PATH_MAX];
	struct vpd_parser *parser;
	const char *vpd_keyword;
	const char *vpd_data;
	char *vpd_vu;
	int ret;

	ret = snprintf(vpd_path, DEV_PATH_MAX, "/sys/bus/pci/devices/%s/vpd",
		       bdf);
	if (ret < 0 || ret >= DEV_PATH_MAX) {
		errno = EINVAL;
		log_debug("Failed to create VPD path for %s", bdf);
		return NULL;
	}

	parser = vpd_parser_create(vpd_path);
	if (!parser)
		return NULL;

	while (true) {
		ret = vpd_parser_next_tag(parser);
		if (ret)
			goto err;

		vpd_tag = vpd_parser_get_resource_tag(parser);
		if (vpd_tag == VPD_PARSER_RESOURCE_TAG_END) {
			log_debug("Could not find VPD large read tag for %s",
				  bdf);
			ret = ENOENT;
			goto err;
		} else if (vpd_tag != VPD_PARSER_RESOURCE_TAG_LARGE_READ) {
			continue;
		}

		while (true) {
			ret = vpd_parser_next_item(parser);
			if (ret == VPD_PARSER_ITEMS_END) {
				log_debug("Could not find VPD VU item for %s",
					  bdf);
				ret = ENOENT;
				goto err;
			} else if (ret) {
				goto err;
			}

			vpd_keyword = vpd_parser_get_item_keyword(parser);
			if (!vpd_keyword) {
				ret = errno;
				goto err;
			}

			if (strcmp(vpd_keyword, VUID_VPD_KEYWORD))
				continue;

			vpd_data = vpd_parser_get_item_data(parser);
			if (!vpd_data) {
				ret = errno;
				goto err;
			}

			vpd_vu = strdup(vpd_data);
			if (!vpd_vu) {
				ret = errno;
				log_debug("Failed to duplicate VPD VU");
				goto err;
			}

			goto out;
		}
	}

out:
	vpd_parser_destroy(parser);

	return vpd_vu;

err:
	vpd_parser_destroy(parser);
	errno = ret;

	return NULL;
}

#define UINT16_ENTRY_SIZE sizeof("0x0000")
static int dd_get_uint16_sysfs_entry(const char *bdf, const char *entry)
{
	char pci_dev_entry_path[DEV_PATH_MAX];
	char entry_str[UINT16_ENTRY_SIZE];
	unsigned long entry_value;
	char *endptr;
	ssize_t rc;
	int ret;
	int fd;

	ret = snprintf(pci_dev_entry_path, DEV_PATH_MAX,
		       "/sys/bus/pci/devices/%s/%s", bdf, entry);
	if (ret < 0 || ret >= DEV_PATH_MAX) {
		log_debug("Failed to create path for PCI device %s entry %s",
			  bdf, entry);
		return -EINVAL;
	}

	fd = open(pci_dev_entry_path, O_RDONLY);
	if (fd < 0) {
		log_debug("Failed to open path %s", pci_dev_entry_path);
		return -errno;
	}

	rc = read(fd, entry_str, sizeof(entry_str));
	if (rc < 0) {
		ret = errno;
		log_debug("Failed to read file %s", pci_dev_entry_path);
		close(fd);
		return -ret;
	} else if (rc < sizeof(entry_str)) {
		log_debug("Read only %zd bytes of file %s", rc,
			  pci_dev_entry_path);
		close(fd);
		return -EINVAL;
	}

	close(fd);

	/* Replace new line with NULL byte terminator */
	entry_str[UINT16_ENTRY_SIZE - 1] = '\0';

	errno = 0;
	entry_value = strtoul(entry_str, &endptr, 0);
	if (errno) {
		log_debug("Failed to parse %s contents of file %s", entry_str,
			  pci_dev_entry_path);
		return -errno;
	}

	if (endptr == entry_str) {
		log_debug("Invalid %s contents of file %s", entry_str,
			  pci_dev_entry_path);
		return -EINVAL;
	}

	if (entry_value > UINT16_MAX) {
		log_debug("%s contents of file %s is too big", entry_str,
			  pci_dev_entry_path);
		return -EINVAL;
	}

	return entry_value;
}

static int dd_get_sysfs_vendor(const char *bdf)
{
	return dd_get_uint16_sysfs_entry(bdf, "vendor");
}

static int dd_get_sysfs_device(const char *bdf)
{
	return dd_get_uint16_sysfs_entry(bdf, "device");
}

#define MLX_VENDOR_ID 0x15b3
#define DATA_DIRECT_DEVICE_ID 0x2100
static int dd_pci_device_is_data_direct(const char *bdf)
{
	int vendor_id;
	int device_id;

	vendor_id = dd_get_sysfs_vendor(bdf);
	if (vendor_id < 0) {
		log_debug("Failed to get vendor ID for %s (errno %d)",
			bdf, -vendor_id);
		return vendor_id;
	}

	if (vendor_id != MLX_VENDOR_ID)
		return 0;

	device_id = dd_get_sysfs_device(bdf);
	if (device_id < 0) {
		log_debug("Failed to get device ID for %s (errno %d)",
			bdf, -device_id);
		return device_id;
	}

	return device_id == DATA_DIRECT_DEVICE_ID;
}

static char *dd_get_data_direct_pci_device_by_vuid(const char *vuid)
{
	char pci_dev_vpd_path[DEV_PATH_MAX];
	struct dirent *dent;
	DIR *pci_devs_dir;
	char *pci_dev_vu;
	char *dd_pci_dev;
	int ret;

	pci_devs_dir = opendir("/sys/bus/pci/devices");
	if (!pci_devs_dir) {
		log_debug("Failed to open PCI devices directory (errno %d)",
			  errno);
		return NULL;
	}

	while ((dent = readdir(pci_devs_dir))) {
		if (!strcmp(".", dent->d_name) || !strcmp("..", dent->d_name))
			continue;

		ret = dd_pci_device_is_data_direct(dent->d_name);
		if (ret < 0) {
			ret = -ret;
			goto err;
		} else if (!ret) {
			continue;
		}

		ret = snprintf(pci_dev_vpd_path, DEV_PATH_MAX,
			       "/sys/bus/pci/devices/%s/vpd", dent->d_name);
		if (ret < 0 || ret >= DEV_PATH_MAX) {
			ret = EINVAL;
			log_debug("Failed to create PCI device VPD path for %s",
				  dent->d_name);
			goto err;
		}

		if (access(pci_dev_vpd_path, F_OK)) {
			/* The PCI device doesn't have VPD entry */
			continue;
		}

		pci_dev_vu = dd_pci_device_get_vu(dent->d_name);
		if (!pci_dev_vu) {
			ret = errno;
			goto err;
		}

		if (!strcmp(pci_dev_vu, vuid)) {
			dd_pci_dev = strdup(dent->d_name);
			if (!dd_pci_dev) {
				ret = errno;
				log_debug(
					"Failed to duplicate data direct device BDF %s",
					dent->d_name);
				free(pci_dev_vu);
				goto err;
			}

			free(pci_dev_vu);
			closedir(pci_devs_dir);

			return dd_pci_dev;
		}

		free(pci_dev_vu);
		pci_dev_vu = NULL;
	}

	ret = ENOENT;
	log_debug("Could not find data direct device with VUID %s", vuid);

err:
	closedir(pci_devs_dir);
	errno = ret;

	return NULL;
}

#define VUID_LEN_MAX 128
#define VUID_SIZE_MAX (VUID_LEN_MAX + 1) /* + 1 for terminating NULL byte */
static char *dd_query_data_direct_vuid(struct mlxfwctl_device *fcdev,
				       uint16_t vhca_id)
{
	enum fwctl_rpc_scope scope = FWCTL_RPC_CONFIGURATION;
	uint32_t out[MLXFWCTL_ST_SZ_DW(query_vuid_out)] = {};
	uint32_t in[MLXFWCTL_ST_SZ_DW(query_vuid_in)] = {};
	void *vuid_cmd_base;
	char *vuid;
	int ret;

	MLXFWCTL_SET(query_vuid_in, in, opcode, MLX5_CMD_OP_QUERY_VUID);
	MLXFWCTL_SET(query_vuid_in, in, data_direct, 1);
	MLXFWCTL_SET(query_vuid_in, in, vhca_id, vhca_id);

	ret = mlxfwctl_device_rpc(fcdev, scope, in, sizeof(in), out,
				  sizeof(out));
	if (ret) {
		errno = ret;
		return NULL;
	}

	vuid = calloc(1, VUID_SIZE_MAX);
	if (!vuid)
		return NULL;

	vuid_cmd_base = MLXFWCTL_ADDR_OF(query_vuid_out, out, vuid);
	memcpy(vuid, vuid_cmd_base, VUID_LEN_MAX);

	log_extra_debug("Data Direct VUID: %s", vuid);

	return vuid;
}

static void *dd_query_hca_caps(struct mlxfwctl_device *fcdev,
			       bool other_function, uint16_t function_id,
			       uint16_t cap_type, uint8_t cap_mode)
{
	size_t out_size = MLXFWCTL_ST_SZ_BYTES(query_hca_cap_out);
	uint32_t in[MLXFWCTL_ST_SZ_DW(query_hca_cap_in)] = {};
	enum fwctl_rpc_scope scope = FWCTL_RPC_CONFIGURATION;
	uint16_t opmod;
	void *out;
	int ret;

	out = calloc(1, out_size);
	if (!out)
		return NULL;

	opmod = cap_type | (cap_mode & 0x1);
	MLXFWCTL_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	MLXFWCTL_SET(query_hca_cap_in, in, op_mod, opmod);
	if (other_function) {
		MLXFWCTL_SET(query_hca_cap_in, in, other_function, 1);
		MLXFWCTL_SET(query_hca_cap_in, in, function_id, function_id);
	}

	ret = mlxfwctl_device_rpc(fcdev, scope, in, sizeof(in), out, out_size);
	if (ret) {
		free(out);
		errno = ret;
		return NULL;
	}

	return out;
}

static int dd_query_vhca_id(struct mlxfwctl_device *fcdev, uint16_t *vhca_id)
{
	void *caps;

	caps = dd_query_hca_caps(fcdev, false, 0,
				 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE,
				 MLX5_HCA_CAP_OPMOD_GET_CUR);
	if (!caps)
		return errno;

	*vhca_id = MLXFWCTL_GET(query_hca_cap_out, caps,
				capability.cmd_hca_cap.vhca_id);

	free(caps);

	return 0;
}

static int dd_can_query_data_direct_vuid(struct mlxfwctl_device *fcdev)
{
	uint32_t tool_partial_cap;
	void *caps;

	caps = dd_query_hca_caps(fcdev, false, 0,
				 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE_CAP_2,
				 MLX5_HCA_CAP_OPMOD_GET_CUR);
	if (!caps)
		return -errno;

	tool_partial_cap =
		MLXFWCTL_GET(query_hca_cap_out, caps,
			     capability.cmd_hca_cap_2.tool_partial_cap);

	free(caps);

	return !!(tool_partial_cap &
		  MLX5_TOOL_PARTIAL_CAP_QUERY_VUID_DIRECT_DATA);
}

char *dd_get_data_direct_device(const char *device)
{
	struct mlxfwctl_device_info *info;
	struct mlxfwctl_device *fcdev;
	char *data_direct_device;
	uint16_t vhca_id = 0;
	char *vuid;
	int ret;

	info = mlxfwctl_device_info_get(device);
	if (!info) {
		log_err("Failed to get device info for %s (errno %d)", device,
			errno);
		return NULL;
	}

	fcdev = mlxfwctl_device_open(info->device_name);
	if (!fcdev) {
		log_err("Failed to open fwctl device %s (errno %d)",
			info->device_name, errno);
		ret = errno;
		goto err;
	}

	ret = dd_can_query_data_direct_vuid(fcdev);
	if (ret < 0) {
		ret = -ret;
		log_err("Failed to check data direct VUID query support for %s (errno %d)",
			device, ret);
		goto err_close_device;
	}
	if (!ret) {
		ret = EOPNOTSUPP;
		goto err_close_device;
	}

	ret = dd_query_vhca_id(fcdev, &vhca_id);
	if (ret) {
		log_err("Failed to query VHCA ID for %s (errno %d)",
			device, ret);
		goto err_close_device;
	}

	vuid = dd_query_data_direct_vuid(fcdev, vhca_id);
	if (!vuid) {
		log_err("Failed to query data direct VUID for %s (errno %d)",
			device, errno);
		ret = errno;
		goto err_close_device;
	}

	data_direct_device = dd_get_data_direct_pci_device_by_vuid(vuid);
	if (!data_direct_device) {
		log_err("Failed to get data direct device for %s (errno %d)",
			device, errno);
		ret = errno;
		goto err_free_vuid;
	}

	free(vuid);
	mlxfwctl_device_close(fcdev);
	mlxfwctl_device_info_free(info);

	return data_direct_device;

err_free_vuid:
	free(vuid);

err_close_device:
	mlxfwctl_device_close(fcdev);

err:
	mlxfwctl_device_info_free(info);
	errno = ret;

	return NULL;
}

#define DD_ARRAY_SIZE_DEFAULT 32
static struct dd_array *dd_array_alloc(void)
{
	struct dd_array *dd_arr;

	dd_arr = calloc(1, sizeof(*dd_arr));
	if (!dd_arr)
		return NULL;

	dd_arr->entries =
		calloc(DD_ARRAY_SIZE_DEFAULT, sizeof(*dd_arr->entries));
	if (!dd_arr->entries) {
		free(dd_arr);
		return NULL;
	}

	dd_arr->size = DD_ARRAY_SIZE_DEFAULT;
	dd_arr->len = 0;

	return dd_arr;
}

static void dd_array_free_entry(struct dd_array_entry *entry)
{
	free(entry->device);
	free(entry->data_direct_device);
}

void dd_array_free(struct dd_array *dd_arr)
{
	int i;

	for (i = 0; i < dd_arr->len; i++)
		dd_array_free_entry(&dd_arr->entries[i]);

	free(dd_arr->entries);
	free(dd_arr);
}

static int dd_array_resize(struct dd_array *dd_arr, size_t new_size)
{
	struct dd_array_entry *new_entries;

	new_entries = calloc(new_size, sizeof(*new_entries));
	if (!new_entries) {
		log_debug("Failed to resize dd_array to size %zu", new_size);
		return errno;
	}

	memcpy(new_entries, dd_arr->entries,
	       dd_arr->len * sizeof(*dd_arr->entries));
	free(dd_arr->entries);
	dd_arr->entries = new_entries;
	dd_arr->size = new_size;

	return 0;
}

static int dd_array_add_entry(struct dd_array *dd_arr, const char *device,
			      const char *data_direct_device)
{
	char *dup_data_direct_device;
	char *dup_device;
	int ret;

	dup_device = strdup(device);
	if (!dup_device)
		return errno;

	dup_data_direct_device = strdup(data_direct_device);
	if (!dup_data_direct_device) {
		ret = errno;
		goto err;
	}

	if (dd_arr->len == dd_arr->size) {
		ret = dd_array_resize(dd_arr, dd_arr->size * 2);
		if (ret)
			goto err_free_data_direct;
	}

	dd_arr->entries[dd_arr->len].device = dup_device;
	dd_arr->entries[dd_arr->len].data_direct_device =
		dup_data_direct_device;
	dd_arr->len++;

	return 0;

err_free_data_direct:
	free(dup_data_direct_device);

err:
	free(dup_device);

	return ret;
}

#define CX8_DEVICE_ID 0x1023
#define MLX5_VF_ID 0x101e
#define PCI_DEVICE_CX8 1
#define PCI_DEVICE_VF 2
static int dd_pci_device_is_cx8_or_vf(const char *bdf)
{
	int vendor_id;
	int device_id;

	vendor_id = dd_get_sysfs_vendor(bdf);
	if (vendor_id < 0)
		return vendor_id;

	if (vendor_id != MLX_VENDOR_ID)
		return 0;

	device_id = dd_get_sysfs_device(bdf);
	if (device_id < 0)
		return device_id;

	if (device_id == CX8_DEVICE_ID)
		return PCI_DEVICE_CX8;

	if (device_id == MLX5_VF_ID) {
		char vf_physfn_path[DEV_PATH_MAX];
		int ret;

		/*
		 * We only want VFs that are inside VMs. If the VF has no physfn
		 * entry, it's inside a VM.
		 */
		ret = snprintf(vf_physfn_path, DEV_PATH_MAX,
			       "/sys/bus/pci/devices/%s/physfn", bdf);
		if (ret < 0 || ret >= DEV_PATH_MAX) {
			log_debug("Failed to create physfn path for VF %s",
				  bdf);
			return -EINVAL;
		}

		if (access(vf_physfn_path, F_OK))
			return PCI_DEVICE_VF;
	}

	return 0;
}

#define DEV_NAME_MAX 32
struct dd_array *dd_get_all_data_direct_devices(void)
{
	char device[DEV_NAME_MAX];
	char *data_direct_device;
	struct dd_array *dd_arr;
	struct dirent *dent;
	DIR *mlx5_core_dir;
	int dev_type;
	int ret = 0;

	dd_arr = dd_array_alloc();
	if (!dd_arr) {
		log_err("Failed to allocate device array");
		return NULL;
	}

	mlx5_core_dir = opendir("/sys/bus/pci/drivers/mlx5_core");
	if (!mlx5_core_dir) {
		if (errno == ENOENT)
			return dd_arr;

		ret = errno;
		log_err("Failed to open mlx5_core directory (errno %d)", ret);
		goto err;
	}

	while ((dent = readdir(mlx5_core_dir))) {
		if (!util_is_bdf_format(dent->d_name))
			continue;

		dev_type = dd_pci_device_is_cx8_or_vf(dent->d_name);
		if (dev_type < 0) {
			ret = -dev_type;
			log_err("Failed to check device type of %s (errno %d)",
				dent->d_name, ret);
			goto err_close_dir;
		} else if (!dev_type) {
			continue;
		}

		ret = snprintf(device, DEV_NAME_MAX, "pci/%s", dent->d_name);
		if (ret < 0 || ret >= DEV_NAME_MAX) {
			ret = EINVAL;
			log_err("Failed to create device name for %s",
				dent->d_name);
			goto err_close_dir;
		}

		data_direct_device = dd_get_data_direct_device(device);
		if (!data_direct_device) {
			if (errno == EOPNOTSUPP)
				continue;

			ret = errno;
			goto err_close_dir;
		}

		ret = dd_array_add_entry(dd_arr, device, data_direct_device);
		if (ret) {
			log_err("Failed to add entry for %s (errno %d)",
				device, ret);
			goto err_free_data_direct;
		}

		if (dev_type == PCI_DEVICE_CX8) {
			/* Add entry for VFs/SFs of the PF */
			ret = snprintf(device, DEV_NAME_MAX, "pci/%s/*",
				       dent->d_name);
			if (ret < 0 || ret >= DEV_NAME_MAX) {
				ret = EINVAL;
				log_err("Failed to create rep device name for %s",
					dent->d_name);
				goto err_free_data_direct;
			}

			ret = dd_array_add_entry(dd_arr, device,
						 data_direct_device);
			if (ret) {
				log_err("Failed to add entry for %s (errno %d)",
					device, ret);
				goto err_free_data_direct;
			}
		}

		free(data_direct_device);
		data_direct_device = NULL;
	}

	closedir(mlx5_core_dir);

	return dd_arr;

err_free_data_direct:
	free(data_direct_device);

err_close_dir:
	closedir(mlx5_core_dir);

err:
	dd_array_free(dd_arr);
	errno = ret;

	return NULL;
}

int dd_get_data_direct_state(const char *device, bool *state)
{
	struct mlxfwctl_device_info *info;
	struct mlxfwctl_device *fcdev;
	int ret = 0;
	void *caps;

	info = mlxfwctl_device_info_get(device);
	if (!info) {
		log_err("Failed to get device info for %s (errno %d)",
			device, errno);
		return errno;
	}

	fcdev = mlxfwctl_device_open(info->device_name);
	if (!fcdev) {
		log_err("Failed to open fwctl device %s (errno %d)",
			info->device_name, errno);
		ret = errno;
		goto out;
	}

	if (info->type == MLXFWCTL_DEVICE_TYPE_DEV) {
		log_err("Device can't get data direct state for itself");
		ret = EINVAL;
		goto out_close_device;
	}

	caps = dd_query_hca_caps(fcdev, true, info->function_id,
				 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE,
				 MLX5_HCA_CAP_OPMOD_GET_MAX);
	if (!caps) {
		log_err("Failed to query HCA capabilities for %s (errno %d)",
			device, errno);
		ret = errno;
		goto out_close_device;
	}

	*state = MLXFWCTL_GET(query_hca_cap_out, caps,
			      capability.cmd_hca_cap.data_direct);
	free(caps);

out_close_device:
	mlxfwctl_device_close(fcdev);

out:
	mlxfwctl_device_info_free(info);

	return ret;
}

static int dd_check_query_vuid_enabled(struct mlxfwctl_device *fcdev,
				       uint16_t function_id)
{
	bool query_vuid;
	void *caps;

	caps = dd_query_hca_caps(fcdev, true, function_id,
				 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE_CAP_2,
				 MLX5_HCA_CAP_OPMOD_GET_MAX);
	if (!caps)
		return errno;

	query_vuid = MLXFWCTL_GET(query_hca_cap_out, caps,
				  capability.cmd_hca_cap_2.query_vuid);
	free(caps);

	if (!query_vuid)
		log_warn(
			"'query_vuid' capability is not set, data direct cannot work");

	return 0;
}

#define SYNDROME_SET_CAP_INVALID 0xbeac6e
static int dd_set_data_direct_state_do(struct mlxfwctl_device *fcdev,
				       uint16_t function_id, bool state)
{
	uint32_t out[MLXFWCTL_ST_SZ_DW(set_hca_cap_out)] = {};
	enum fwctl_rpc_scope scope = FWCTL_RPC_CONFIGURATION;
	uint32_t in[MLXFWCTL_ST_SZ_DW(set_hca_cap_in)] = {};
	uint16_t opmod;
	void *caps_out;
	void *set_caps;
	void *caps;
	int ret;

	caps_out = dd_query_hca_caps(fcdev, true, function_id,
				     MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE,
				     MLX5_HCA_CAP_OPMOD_GET_MAX);
	if (!caps_out)
		return errno;

	if (state == MLXFWCTL_GET(query_hca_cap_out, caps_out,
				  capability.cmd_hca_cap.data_direct)) {
		ret = 0;
		goto out;
	}

	if (state) {
		ret = dd_check_query_vuid_enabled(fcdev, function_id);
		if (ret)
			goto out;
	}

	caps = MLXFWCTL_ADDR_OF(query_hca_cap_out, caps_out, capability);
	set_caps = MLXFWCTL_ADDR_OF(set_hca_cap_in, in, capability);
	memcpy(set_caps, caps, MLXFWCTL_UN_SZ_BYTES(hca_cap_union));

	MLXFWCTL_SET(set_hca_cap_in, in, capability.cmd_hca_cap.data_direct,
		     state);

	opmod = MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE;
	MLXFWCTL_SET(set_hca_cap_in, in, opcode, MLX5_CMD_OP_SET_HCA_CAP);
	MLXFWCTL_SET(set_hca_cap_in, in, op_mod, opmod);
	MLXFWCTL_SET(set_hca_cap_in, in, other_function, 1);
	MLXFWCTL_SET(set_hca_cap_in, in, function_id, function_id);

	ret = mlxfwctl_device_rpc(fcdev, scope, in, sizeof(in), out,
				  sizeof(out));
	if (ret == EREMOTEIO) {
		uint32_t syndrome;

		syndrome = MLXFWCTL_GET(set_hca_cap_out, out, syndrome);
		if (syndrome == SYNDROME_SET_CAP_INVALID)
			ret = EBUSY;
	}

out:
	free(caps_out);

	return ret;
}

int dd_set_data_direct_state(const char *device, bool state)
{
	struct mlxfwctl_device_info *info;
	struct mlxfwctl_device *fcdev;
	int ret;

	info = mlxfwctl_device_info_get(device);
	if (!info) {
		log_err("Failed to get device info for %s (errno %d)",
			device, errno);
		return errno;
	}

	if (info->type == MLXFWCTL_DEVICE_TYPE_DEV) {
		log_err("Device can't set data direct for itself");
		ret = EINVAL;
		goto out;
	}

	fcdev = mlxfwctl_device_open(info->device_name);
	if (!fcdev) {
		log_err("Failed to open fwctl device %s (errno %d)",
			info->device_name, errno);
		ret = errno;
		goto out;
	}

	ret = dd_set_data_direct_state_do(fcdev, info->function_id, state);
	if (ret == EBUSY)
		log_err("Can't set data direct because device %s is already initialized",
			device);
	else if (ret)
		log_err("Failed to set data direct for %s (errno %d)", device,
			ret);

	mlxfwctl_device_close(fcdev);

out:
	mlxfwctl_device_info_free(info);

	return ret;
}
