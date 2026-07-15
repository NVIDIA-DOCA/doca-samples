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

#ifndef MLXFWCTL_H
#define MLXFWCTL_H

#include <endian.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/types.h>

#include "kernel-headers/fwctl/fwctl.h"

#define __mlxfwctl_nullp(typ) ((struct mlx5_ifc_##typ##_bits *)NULL)
#define __mlxfwctl_st_sz_bits(typ) sizeof(struct mlx5_ifc_##typ##_bits)
#define __mlxfwctl_bit_sz(typ, fld) sizeof(__mlxfwctl_nullp(typ)->fld)
#define __mlxfwctl_bit_off(typ, fld) offsetof(struct mlx5_ifc_##typ##_bits, fld)
#define __mlxfwctl_dw_off(bit_off) ((bit_off) / 32)
#define __mlxfwctl_64_off(bit_off) ((bit_off) / 64)
#define __mlxfwctl_dw_bit_off(bit_sz, bit_off)                                 \
	(32 - (bit_sz) - ((bit_off)&0x1f))
#define __mlxfwctl_mask(bit_sz) ((uint32_t)((1ull << (bit_sz)) - 1))
#define __mlxfwctl_dw_mask(bit_sz, bit_off)                                    \
	(__mlxfwctl_mask(bit_sz) << __mlxfwctl_dw_bit_off(bit_sz, bit_off))

#define MLXFWCTL_FLD_SZ_BYTES(typ, fld) (__mlxfwctl_bit_sz(typ, fld) / 8)
#define MLXFWCTL_ST_SZ_BYTES(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 8)
#define MLXFWCTL_ST_SZ_DW(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 32)
#define MLXFWCTL_ST_SZ_QW(typ) (sizeof(struct mlx5_ifc_##typ##_bits) / 64)
#define MLXFWCTL_UN_SZ_BYTES(typ) (sizeof(union mlx5_ifc_##typ##_bits) / 8)
#define MLXFWCTL_UN_SZ_DW(typ) (sizeof(union mlx5_ifc_##typ##_bits) / 32)
#define MLXFWCTL_BYTE_OFF(typ, fld) (__mlxfwctl_bit_off(typ, fld) / 8)
#define MLXFWCTL_ADDR_OF(typ, p, fld)                                          \
	((unsigned char *)(p) + MLXFWCTL_BYTE_OFF(typ, fld))

static inline void _mlxfwctl_set(void *p, uint32_t value, size_t bit_off,
				 size_t bit_sz)
{
	__be32 *fld = (__be32 *)(p) + __mlxfwctl_dw_off(bit_off);
	uint32_t dw_mask = __mlxfwctl_dw_mask(bit_sz, bit_off);
	uint32_t mask = __mlxfwctl_mask(bit_sz);

	*fld = htobe32(
		(be32toh(*fld) & (~dw_mask)) |
		((value & mask) << __mlxfwctl_dw_bit_off(bit_sz, bit_off)));
}

#define MLXFWCTL_SET(typ, p, fld, v)                                           \
	_mlxfwctl_set(p, v, __mlxfwctl_bit_off(typ, fld),                      \
		      __mlxfwctl_bit_sz(typ, fld))

static inline uint32_t _mlxfwctl_get(const void *p, size_t bit_off,
				     size_t bit_sz)
{
	return ((be32toh(*((const __be32 *)(p) + __mlxfwctl_dw_off(bit_off))) >>
		 __mlxfwctl_dw_bit_off(bit_sz, bit_off)) &
		__mlxfwctl_mask(bit_sz));
}

#define MLXFWCTL_GET(typ, p, fld)                                              \
	_mlxfwctl_get(p, __mlxfwctl_bit_off(typ, fld),                         \
		      __mlxfwctl_bit_sz(typ, fld))

static inline void _mlxfwctl_set64(void *p, uint64_t v, size_t bit_off)
{
	*((__be64 *)(p) + __mlxfwctl_64_off(bit_off)) = htobe64(v);
}

#define MLXFWCTL_SET64(typ, p, fld, v)                                         \
	_mlxfwctl_set64(p, v, __mlxfwctl_bit_off(typ, fld))

static inline uint64_t _mlxfwctl_get64(const void *p, size_t bit_off)
{
	return be64toh(*((const __be64 *)(p) + __mlxfwctl_64_off(bit_off)));
}

#define MLXFWCTL_GET64(typ, p, fld)                                            \
	_mlxfwctl_get64(p, __mlxfwctl_bit_off(typ, fld))

#define MLXFWCTL_ARRAY_SET64(typ, p, fld, idx, v)                              \
	do {                                                                   \
		MLXFWCTL_SET64(typ, p, fld[idx], v);                           \
	} while (0)

struct mlxfwctl_device;

/**
 * mlxfwctl_device_open:
 * @name: The fwctl device name
 *
 * Opens the fwctl device with the given name.
 *
 * Returns a pointer to the fwctl device on success. Returns NULL and sets
 * errno with an appropriate error code on failure. The caller must close the
 * opened device with mlxfwctl_device_close().
 */
struct mlxfwctl_device *mlxfwctl_device_open(const char *name);

/**
 * mlxfwctl_device_close:
 * @fcdev: The fwctl device to close
 *
 * Closes @fcdev fwctl device.
 */
void mlxfwctl_device_close(struct mlxfwctl_device *fcdev);

/**
 * mlxfwctl_device_rpc:
 * @fcdev: The fwctl device to issue the RPC on
 * @scope: The scope of the RPC
 * @in: Pointer to the input buffer of the RPC
 * @in_len: The length of @in
 * @out: Pointer to the output buffer of the RPC
 * @out_len: The length of @out
 *
 * Issues an RPC on the fwctl deivce @fcdev using @in as input buffer and @out
 * as output buffer.
 *
 * Returns 0 on success and an errno error code on failure.
 */
int mlxfwctl_device_rpc(struct mlxfwctl_device *fcdev,
			enum fwctl_rpc_scope scope, void *in, uint32_t in_len,
			void *out, uint32_t out_len);

enum mlxfwctl_device_type {
	MLXFWCTL_DEVICE_TYPE_DEV,
	MLXFWCTL_DEVICE_TYPE_REP,
};

/**
 * mlxfwctl_device_info:
 * @type: The type of the device
 * @device_name: The fwctl device name of the device
 * @function_id: The function ID of the device. This is only valid for a REP
 * 		 device type.
 * @vhca_id: The VHCA ID of the device. This is only valid for a REP device
 * 	     type.
 */
struct mlxfwctl_device_info {
	enum mlxfwctl_device_type type;
	char *device_name;
	uint16_t function_id;
	uint16_t vhca_id;
};

/**
 * mlxfwctl_device_info_get:
 * @device: The device to get its info. This can be a devlink PCI device
 *	    identifier (e.g., pci/0000:08:00.0) or a devlink representor
 *	    device identifier (e.g., pci/0000:08:00.0/32768).
 *
 * Gets the info of the given device.
 *
 * Returns a pointer to the device info on success. Returns NULL and sets errno
 * with an appropriate error code on failure. The caller must free the returned
 * info with mlxfwctl_device_info_free().
 */
struct mlxfwctl_device_info *mlxfwctl_device_info_get(const char *device);

/**
 * mlxfwctl_device_info_free:
 * @info: The device info to free
 *
 * Frees the device info allocated by mlxfwctl_device_info_get().
 */
void mlxfwctl_device_info_free(struct mlxfwctl_device_info *info);

#endif
