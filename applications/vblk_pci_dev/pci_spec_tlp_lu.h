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

#ifndef PCI_SPEC_TLP_LU_H_
#define PCI_SPEC_TLP_LU_H_

#include <doca_bitfield.h>

/* PCIe TLP Types */
enum tlp_type {
	TLP_TYPE_MEM_READ = 0x00,
	TLP_TYPE_MEM_READ_LOCKED = 0x01,
	TLP_TYPE_MEM_WRITE = 0x00,
	TLP_TYPE_IO_READ = 0x02,
	TLP_TYPE_IO_WRITE = 0x02,
	TLP_TYPE_CFG_READ_TYPE0 = 0x04,
	TLP_TYPE_CFG_WRITE_TYPE0 = 0x04,
	TLP_TYPE_CFG_READ_TYPE1 = 0x05,
	TLP_TYPE_CFG_WRITE_TYPE1 = 0x05,
	TLP_TYPE_MSG_TO_ROOT = 0x10,
	TLP_TYPE_MSG_BY_ADDR = 0x11,
	TLP_TYPE_MSG_BY_ID = 0x12,
	TLP_TYPE_MSG_FROM_ROOT = 0x13,
	TLP_TYPE_MSG_LOCAL = 0x14,
	TLP_TYPE_MSG_GATHER = 0x15,
	TLP_TYPE_COMPLETION = 0x0A,
	TLP_TYPE_COMPLETION_DATA = 0x0A,
	TLP_TYPE_COMPLETION_LOCKED = 0x0B,
	TLP_TYPE_COMPLETION_LOCKED_DATA = 0x0B
};

/* PCIe TLP Header Format Types */
enum tlp_format {
	TLP_FMT_3DW_NO_DATA = 0x0,   /* 3DW header, no data */
	TLP_FMT_4DW_NO_DATA = 0x1,   /* 4DW header, no data */
	TLP_FMT_3DW_WITH_DATA = 0x2, /* 3DW header, with data */
	TLP_FMT_4DW_WITH_DATA = 0x3, /* 4DW header, with data */
	TLP_FMT_TLP_PREFIX = 0x4     /* TLP prefix */
};

/* Completion Status */
enum tlp_completion_status {
	TLP_CPL_STATUS_SC = 0x0,  /* Successful Completion */
	TLP_CPL_STATUS_UR = 0x1,  /* Unsupported Request */
	TLP_CPL_STATUS_CRS = 0x2, /* Configuration Request Retry Status */
	TLP_CPL_STATUS_CA = 0x4	  /* Completer Abort */
};

enum tlp_req_type {
	TLP_REQ_TYPE_INVALID = 0,
	TLP_REQ_TYPE_MEMORY_READ,
	TLP_REQ_TYPE_MEMORY_WRITE,
	TLP_REQ_TYPE_IO_READ,
	TLP_REQ_TYPE_IO_WRITE,
	TLP_REQ_TYPE_CONFIG_READ_TYPE_0,
	TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0,
	TLP_REQ_TYPE_CONFIG_READ_TYPE_1,
	TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1,
};

/* Note that incoming TLPs are in the big endian format */
#define GET_TLP_REQ_FMT(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 29), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_TYPE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(28, 24), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_FMT_AND_TYPE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 24), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_TAG9(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(23, 23), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_TAG8(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(19, 19), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_LENGTH(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(9, 0), ((uint32_t *)(tlp_req))[0])
#define GET_TLP_REQ_REQ_ID(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 16), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_TAG(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(15, 8), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_LAST_DW_BE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(7, 4), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_FIRST_DW_BE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(3, 0), ((uint32_t *)(tlp_req))[1])
#define GET_TLP_REQ_BUS(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(31, 24), ((uint32_t *)(tlp_req))[2])
#define GET_TLP_REQ_DEVICE(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(23, 19), ((uint32_t *)(tlp_req))[2])
#define GET_TLP_REQ_FUNCTION(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(18, 16), ((uint32_t *)(tlp_req))[2])
#define GET_TLP_REQ_EXT_REG_NUM(tlp_req) DOCA_BE32_GET(DOCA_BE32_GENMASK(11, 2), ((uint32_t *)(tlp_req))[2])

enum tlp_req_type tlp_req_get_type(struct doca_devemu_pci_tlp_channel_req *tlp_req);

#endif
