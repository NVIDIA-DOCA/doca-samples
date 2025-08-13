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

#ifndef MLX5_IFC_H
#define MLX5_IFC_H

#include <stdint.h>

#define u8 uint8_t

enum {
	MLX5_CMD_OP_QUERY_HCA_CAP = 0x100,
};

enum mlx5_cap_mode {
	MLX5_HCA_CAP_OPMOD_GET_MAX = 0,
	MLX5_HCA_CAP_OPMOD_GET_CUR = 1,
};

enum {
	MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE = 0x0 << 1,
};

struct mlx5_ifc_cmd_hca_cap_bits {
	u8 reserved_at_0[0x20];

	u8 reserved_at_20[0x10];
	u8 vhca_id[0x10];

	u8 reserved_at_40[0x7c0];
};

union mlx5_ifc_hca_cap_union_bits {
	struct mlx5_ifc_cmd_hca_cap_bits cmd_hca_cap;
	u8 reserved_at_0[0x8000];
};

struct mlx5_ifc_query_hca_cap_out_bits {
	u8 status[0x8];
	u8 reserved_at_8[0x18];

	u8 syndrome[0x20];

	u8 reserved_at_40[0x40];

	union mlx5_ifc_hca_cap_union_bits capability;
};

struct mlx5_ifc_query_hca_cap_in_bits {
	u8 opcode[0x10];
	u8 reserved_at_10[0x10];

	u8 reserved_at_20[0x10];
	u8 op_mod[0x10];

	u8 other_function[0x1];
	u8 reserved_at_41[0xf];
	u8 function_id[0x10];

	u8 reserved_at_60[0x20];
};

struct mlx5_ifc_mbox_out_bits {
	u8         status[0x8];
	u8         reserved_at_8[0x18];

	u8         syndrome[0x20];

	u8         reserved_at_40[0x40];
};

struct mlx5_ifc_mbox_in_bits {
	u8         opcode[0x10];
	u8         uid[0x10];

	u8         reserved_at_20[0x10];
	u8         op_mod[0x10];

	u8         reserved_at_40[0x40];
};

#undef u8

#endif
