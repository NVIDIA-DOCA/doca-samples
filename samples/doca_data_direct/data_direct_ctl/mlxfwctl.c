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
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include "mlx5_ifc.h"
#include "mlxfwctl.h"

#include "kernel-headers/fwctl/fwctl.h"
#include "kernel-headers/fwctl/mlx5.h"

#include "util.h"

#define mlxfwctl_err(fcdev, format, arg...)                                    \
	log_err("%s: " format, (fcdev)->name, ##arg)

#define mlxfwctl_warn(fcdev, format, arg...)                                   \
	log_warn("%s: " format, (fcdev)->name, ##arg)

#define mlxfwctl_info(fcdev, format, arg...)                                   \
	log_info("%s: " format, (fcdev)->name, ##arg)

#define mlxfwctl_debug(fcdev, format, arg...)                                  \
	log_debug("%s: " format, (fcdev)->name, ##arg)

#define mlxfwctl_extra_debug(fcdev, format, arg...)                            \
	log_extra_debug("%s: " format, (fcdev)->name, ##arg)

struct mlxfwctl_device {
	char *name;
	int fd;
	uint32_t uid;
	uint32_t uctx_caps;
};

static int mlxfwctl_device_query_info(struct mlxfwctl_device *fcdev)
{
	struct fwctl_info_mlx5 fc_info_mlx5 = {};
	struct fwctl_info fc_info = {
		.size = sizeof(fc_info),
		.flags = 0,
		.device_data_len = sizeof(fc_info_mlx5),
		.out_device_data = (uintptr_t)&fc_info_mlx5,
	};
	int ret;

	ret = ioctl(fcdev->fd, FWCTL_INFO, &fc_info);
	if (ret) {
		mlxfwctl_debug(fcdev,
			     "Failed to query fwctl device info (errno %d)",
			     errno);
		return errno;
	}

	if (fc_info.out_device_type != FWCTL_DEVICE_TYPE_MLX5) {
		mlxfwctl_debug(fcdev,
			     "Invalid fwctl device type %u (expected %u)",
			     fc_info.out_device_type, FWCTL_DEVICE_TYPE_MLX5);
		return EINVAL;
	}

	fcdev->uid = fc_info_mlx5.uid;
	fcdev->uctx_caps = fc_info_mlx5.uctx_caps;

	mlxfwctl_debug(fcdev, "fwctl device info: uid %u, uctx_caps 0x%x",
		       fcdev->uid, fcdev->uctx_caps);

	return 0;
}

struct mlxfwctl_device *mlxfwctl_device_open(const char *name)
{
	struct mlxfwctl_device *fcdev;
	char fwctl_path[DEV_PATH_MAX];
	int ret;
	int fd;

	fcdev = calloc(1, sizeof(*fcdev));
	if (!fcdev) {
		log_debug("Failed to allocate mlxfwctl device");
		errno = ENOMEM;
		return NULL;
	}

	fcdev->name = strdup(name);
	if (!fcdev->name) {
		log_debug("Failed to duplicate fwctl device name %s", name);
		ret = ENOMEM;
		goto err;
	}

	ret = snprintf(fwctl_path, DEV_PATH_MAX, "/dev/fwctl/%s", fcdev->name);
	if (ret < 0 || ret >= DEV_PATH_MAX) {
		log_debug("Failed to create fwctl device path for %s", name);
		ret = EINVAL;
		goto err_free_name;
	}

	fd = open(fwctl_path, O_RDWR);
	if (fd < 0) {
		ret = errno;
		log_debug("Failed to open fwctl device %s (errno %d)",
			     fwctl_path, errno);
		goto err_free_name;
	}

	fcdev->fd = fd;
	ret = mlxfwctl_device_query_info(fcdev);
	if (ret)
		goto err_close_fd;

	return fcdev;

err_close_fd:
	close(fcdev->fd);

err_free_name:
	free(fcdev->name);

err:
	free(fcdev);
	errno = ret;

	return NULL;
}

void mlxfwctl_device_close(struct mlxfwctl_device *fcdev)
{
	close(fcdev->fd);
	free(fcdev->name);
	free(fcdev);
}

static const char *scope_to_str(enum fwctl_rpc_scope scope)
{
	switch (scope) {
	case FWCTL_RPC_CONFIGURATION:
		return "configuration";
	case FWCTL_RPC_DEBUG_READ_ONLY:
		return "debug read only";
	case FWCTL_RPC_DEBUG_WRITE:
		return "debug write";
	case FWCTL_RPC_DEBUG_WRITE_FULL:
		return "debug write full";
	default:
		return "unknown";
	}
}

static void mlxfwctl_device_rpc_dump(struct mlxfwctl_device *fcdev,
				     const struct fwctl_rpc *fc_rpc)
{
	const uint32_t *buf;
	int i;

	if (util_log_level < UTIL_LOG_LEVEL_EXTRA_DEBUG)
		return;

	mlxfwctl_extra_debug(fcdev,
			     "fwctl RPC dump: scope %s, in_len %u, out_len %u",
			     scope_to_str(fc_rpc->scope), fc_rpc->in_len,
			     fc_rpc->out_len);

	buf = (uint32_t *)(uintptr_t)fc_rpc->in;
	log_extra_debug("In buffer:");
	for (i = 0; i < fc_rpc->in_len / 4; i += 4)
		log_extra_debug("%04x: %08x %08x %08x %08x", i * 4,
				be32toh(buf[i]), be32toh(buf[i + 1]),
				be32toh(buf[i + 2]), be32toh(buf[i + 3]));
}

int mlxfwctl_device_rpc(struct mlxfwctl_device *fcdev,
			enum fwctl_rpc_scope scope, void *in, uint32_t in_len,
			void *out, uint32_t out_len)
{
	struct fwctl_rpc fc_rpc = {
		.size = sizeof(fc_rpc),
		.scope = scope,
		.in_len = in_len,
		.out_len = out_len,
		.in = (uintptr_t)in,
		.out = (uintptr_t)out,
	};
	int ret;

	mlxfwctl_device_rpc_dump(fcdev, &fc_rpc);

	ret = ioctl(fcdev->fd, FWCTL_RPC, &fc_rpc);
	if (ret) {
		mlxfwctl_debug(fcdev, "fwctl RPC failed (errno %d)", errno);
		return errno;
	}

	if (MLXFWCTL_GET(mbox_out, out, status)) {
		mlxfwctl_debug(
			fcdev,
			"fwctl RPC failed with status (opcode 0x%hx, opmode 0x%hx, "
			"status 0x%hhx, syndrome 0x%x)",
			MLXFWCTL_GET(mbox_in, in, opcode),
			MLXFWCTL_GET(mbox_in, in, op_mod),
			MLXFWCTL_GET(mbox_out, out, status),
			MLXFWCTL_GET(mbox_out, out, syndrome));

		return EREMOTEIO;
	}

	return 0;
}

#define DEV_ID_SIZE_MAX sizeof("pci/0000:00:00.0/0000000000")
static int mlxfwctl_parse_device_id(const char *device, char **bdf,
				    uint32_t *port, bool *port_valid)
{
	char dev[DEV_ID_SIZE_MAX];
	unsigned long port_index;
	char *port_delim;
	char *port_start;
	char *bdf_delim;
	char *bdf_start;
	char *endptr;
	int ret;

	ret = snprintf(dev, DEV_ID_SIZE_MAX, "%s", device);
	if (ret < 0 || ret >= DEV_ID_SIZE_MAX)
		return EINVAL;

	bdf_delim = strchr(dev, '/');
	if (!bdf_delim)
		return EINVAL;

	*bdf_delim = '\0';
	bdf_start = bdf_delim + 1;
	if (strcmp(dev, "pci"))
		return EINVAL;

	port_delim = strchr(bdf_start, '/');
	if (port_delim) {
		*port_delim = '\0';
		port_start = port_delim + 1;
	}

	if (!util_is_bdf_format(bdf_start))
		return EINVAL;

	*bdf = strdup(bdf_start);
	if (!*bdf)
		return ENOMEM;

	if (!port_delim) {
		*port_valid = false;
		return 0;
	}

	errno = 0;
	port_index = strtoul(port_start, &endptr, 10);
	if (errno) {
		ret = errno;
		goto err;
	}

	if (endptr == port_start || *endptr != '\0' ||
	    port_index > UINT32_MAX) {
		ret = EINVAL;
		goto err;
	}

	*port = port_index;
	*port_valid = true;

	return 0;

err:
	free(*bdf);

	return ret;
}

static char *mlxfwctl_get_device_name_by_bdf(const char *bdf)
{
	char fwctl_dev_path[DEV_PATH_MAX];
	char *fwctl_dev_name = NULL;
	struct dirent *dent;
	DIR *fwctl_dev_dir;
	int ret;

	ret = snprintf(fwctl_dev_path, DEV_PATH_MAX,
		       "/sys/bus/pci/devices/%s/fwctl", bdf);
	if (ret < 0 || ret >= DEV_PATH_MAX) {
		log_debug("Failed to create fwctl device path for %s", bdf);
		errno = EINVAL;
		return NULL;
	}

	fwctl_dev_dir = opendir(fwctl_dev_path);
	if (!fwctl_dev_dir) {
		log_debug("Failed to open %s directory", fwctl_dev_path);
		return NULL;
	}
	while ((dent = readdir(fwctl_dev_dir))) {
		if (!strcmp(".", dent->d_name) || !strcmp("..", dent->d_name)) {
			continue;
		}

		fwctl_dev_name = strdup(dent->d_name);
		if (!fwctl_dev_name) {
			closedir(fwctl_dev_dir);
			log_debug(
				"Failed to duplicate fwctl device name %s for %s",
				dent->d_name, bdf);
			errno = ENOMEM;

			return NULL;
		}
		break;
	}
	closedir(fwctl_dev_dir);

	if (!fwctl_dev_name) {
		log_debug("Failed to find fwctl device for %s", bdf);
		errno = ENOENT;
		return NULL;
	}

	return fwctl_dev_name;
}

#define FUNCTION_ID_MASK 0xffff
static void mlxfwctl_device_get_function_id(uint32_t port,
					    uint16_t *function_id)
{
	*function_id = port & FUNCTION_ID_MASK;
}

static void *mlxfwctl_device_query_hca_cap(struct mlxfwctl_device *fcdev,
					   uint16_t function_id)
{
	size_t out_size = MLXFWCTL_ST_SZ_BYTES(query_hca_cap_out);
	uint32_t in[MLXFWCTL_ST_SZ_DW(query_hca_cap_in)] = {};
	enum fwctl_rpc_scope scope = FWCTL_RPC_CONFIGURATION;
	uint16_t opmod;
	void *out;
	int ret;

	out = calloc(1, out_size);
	if (!out) {
		log_debug("Failed to allocate general HCA cap out buffer");
		errno = ENOMEM;
		return NULL;
	}

	opmod = MLX5_HCA_CAP_OPMOD_GET_CUR |
		MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE;
	MLXFWCTL_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	MLXFWCTL_SET(query_hca_cap_in, in, op_mod, opmod);
	MLXFWCTL_SET(query_hca_cap_in, in, other_function, 1);
	MLXFWCTL_SET(query_hca_cap_in, in, function_id, function_id);

	ret = mlxfwctl_device_rpc(fcdev, scope, in, sizeof(in), out, out_size);
	if (ret) {
		free(out);
		errno = ret;
		return NULL;
	}

	return out;
}

static int mlxfwctl_device_get_vhca_id(const char *device, uint16_t function_id,
				       uint16_t *vhca_id)
{
	struct mlxfwctl_device *fcdev;
	void *hca_caps;
	int ret = 0;

	fcdev = mlxfwctl_device_open(device);
	if (!fcdev)
		return errno;

	hca_caps = mlxfwctl_device_query_hca_cap(fcdev, function_id);
	if (!hca_caps) {
		ret = errno;
		goto out;
	}

	*vhca_id = MLXFWCTL_GET(query_hca_cap_out, hca_caps,
				capability.cmd_hca_cap.vhca_id);
	free(hca_caps);

out:
	mlxfwctl_device_close(fcdev);

	return ret;
}

struct mlxfwctl_device_info *mlxfwctl_device_info_get(const char *device)
{
	struct mlxfwctl_device_info *info;
	bool port_valid = false;
	uint32_t port = 0;
	char *bdf;
	int ret;

	info = calloc(1, sizeof(*info));
	if (!info) {
		log_debug("Failed to allocate mlxfwctl device info");
		errno = ENOMEM;
		return NULL;
	}

	ret = mlxfwctl_parse_device_id(device, &bdf, &port, &port_valid);
	if (ret) {
		log_debug("Failed to parse device id %s (errno %d)", device,
			  ret);
		errno = ret;
		goto err;
	}

	info->type = port_valid ? MLXFWCTL_DEVICE_TYPE_REP :
				  MLXFWCTL_DEVICE_TYPE_DEV;

	info->device_name = mlxfwctl_get_device_name_by_bdf(bdf);
	free(bdf);
	if (!info->device_name) {
		log_debug("Failed to get fwctl device name for %s (errno %d)",
			  device, errno);
		goto err;
	}

	if (info->type == MLXFWCTL_DEVICE_TYPE_REP) {
		mlxfwctl_device_get_function_id(port, &info->function_id);

		ret = mlxfwctl_device_get_vhca_id(
			info->device_name, info->function_id, &info->vhca_id);
		if (ret) {
			log_debug("Failed to get VHCA ID for %s (errno %d)",
				  device, ret);
			errno = ret;
			goto err;
		}
	}

	return info;

err:
	free(info);

	return NULL;

}

void mlxfwctl_device_info_free(struct mlxfwctl_device_info *info)
{
	free(info->device_name);
	free(info);
}
