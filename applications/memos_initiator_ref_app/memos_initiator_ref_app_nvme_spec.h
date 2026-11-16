/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

/*
 * The subset of NVMe this app implements. SQE fields are little-endian on the wire and read via
 * memcpy + le*toh, so the accessors are alignment-safe.
 */

#ifndef MEMOS_INITIATOR_REF_APP_NVME_SPEC_H_
#define MEMOS_INITIATOR_REF_APP_NVME_SPEC_H_

#include <endian.h>
#include <stdint.h>
#include <string.h>

/* Admin command opcodes. */
#define NVME_ADMIN_OPC_DELETE_IO_SQ 0x00
#define NVME_ADMIN_OPC_CREATE_IO_SQ 0x01
#define NVME_ADMIN_OPC_GET_LOG_PAGE 0x02
#define NVME_ADMIN_OPC_DELETE_IO_CQ 0x04
#define NVME_ADMIN_OPC_CREATE_IO_CQ 0x05
#define NVME_ADMIN_OPC_IDENTIFY 0x06
#define NVME_ADMIN_OPC_ABORT 0x08
#define NVME_ADMIN_OPC_SET_FEATURES 0x09
#define NVME_ADMIN_OPC_GET_FEATURES 0x0A
#define NVME_ADMIN_OPC_ASYNC_EVENT 0x0C

/*
 * KV command-set opcodes. Store and Retrieve collide numerically with NVM Write and Read; only the
 * namespace's command set tells them apart, which is why a namespace is KV or NVM but never both.
 * Delete and List are rejected with INVALID_OPCODE.
 */
#define NVME_KV_OPC_STORE 0x01
#define NVME_KV_OPC_RETRIEVE 0x02
#define NVME_KV_OPC_LIST 0x06
#define NVME_KV_OPC_DELETE 0x10
#define NVME_KV_OPC_EXIST 0x14

/* Feature identifiers, Set/Get Features CDW10[7:0]. */
#define NVME_FEAT_VOLATILE_WRITE_CACHE 0x06
#define NVME_FEAT_NUMBER_OF_QUEUES 0x07
#define NVME_FEAT_ASYNC_EVENT_CONFIG 0x0B

/*
 * A completion status is SC[7:0] | SCT[10:8]; the library adds the Phase Tag. A bare NVME_SC_* is
 * SCT 0, so anything command-specific must be composed with NVME_STATUS().
 */
#define NVME_SCT_GENERIC 0x0
#define NVME_SCT_CMD_SPECIFIC 0x1
#define NVME_STATUS(sct, sc) ((uint16_t)(((uint16_t)(sct) << 8) | (uint16_t)(sc)))

/* Generic Command Status (SCT 0x0). */
#define NVME_SC_SUCCESS 0x00
#define NVME_SC_INVALID_OPCODE 0x01
#define NVME_SC_INVALID_FIELD 0x02
#define NVME_SC_DATA_XFER_ERROR 0x04
#define NVME_SC_INTERNAL_ERROR 0x06
#define NVME_SC_CMD_ABORTED_SQ_DELETION 0x08
#define NVME_SC_INVALID_NS 0x0B
#define NVME_SC_NS_NOT_READY 0x82

/* KV Command Set status codes. Generic (SCT 0x0) per NVMe KV Command Set spec 1.4, section 3.1 and Figure 5. */
#define NVME_SC_KV_CAPACITY_EXCEEDED 0x81
#define NVME_SC_KV_INVALID_VALUE_SIZE 0x85
#define NVME_SC_KV_INVALID_KEY_SIZE 0x86
#define NVME_SC_KV_KEY_NOT_EXIST 0x87
#define NVME_SC_KV_UNRECOVERED_ERROR 0x88
#define NVME_SC_KV_KEY_EXISTS 0x89

/* Command Specific Status (SCT 0x1). */
#define NVME_SC_CQ_INVALID 0x00		    /* Create IO SQ: the referenced CQ does not exist */
#define NVME_SC_INVALID_QID 0x01	    /* Create/Delete IO queue: invalid or duplicate qid */
#define NVME_SC_INVALID_QSIZE 0x02	    /* Create IO queue: queue size out of range */
#define NVME_SC_AER_LIMIT_EXCEEDED 0x05	    /* Async Event Request: too many outstanding */
#define NVME_SC_INVALID_QUEUE_DELETION 0x0C /* Delete IO CQ: a SQ is still associated with it */

/* Create/Delete IO Submission/Completion Queue command fields. */
#define NVME_CREATE_IOQ_CDW10_QID(cdw10) ((uint16_t)((cdw10)&0xFFFFu))
#define NVME_CREATE_IOQ_CDW10_QSIZE(cdw10) ((uint16_t)(((cdw10) >> 16) & 0xFFFFu)) /* 0-based */
#define NVME_CREATE_IOQ_CDW11_PC(cdw11) ((cdw11)&0x1u)
#define NVME_CREATE_IOCQ_CDW11_IEN(cdw11) (((cdw11) >> 1) & 0x1u)
#define NVME_CREATE_IOCQ_CDW11_IV(cdw11) ((uint16_t)(((cdw11) >> 16) & 0xFFFFu))
#define NVME_CREATE_IOSQ_CDW11_QPRIO(cdw11) (((cdw11) >> 1) & 0x3u)
#define NVME_CREATE_IOSQ_CDW11_CQID(cdw11) ((uint16_t)(((cdw11) >> 16) & 0xFFFFu))
#define NVME_DELETE_IOQ_CDW10_QID(cdw10) ((uint16_t)((cdw10)&0xFFFFu))

/* CDW0.PSDT (bits [15:14]): 0 selects PRPs, non-zero selects an SGL (unsupported by this app). */
#define NVME_PSDT_PRP 0x0

/* SQE is fixed at 64 bytes for the NVM command set. */
#define NVME_SQE_SIZE 64

/* Identify CNS values (CDW10[7:0]) handled by this app. */
#define NVME_IDENTIFY_CNS_NS 0x00
#define NVME_IDENTIFY_CNS_CTRL 0x01
#define NVME_IDENTIFY_CNS_ACTIVE_NS_LIST 0x02
#define NVME_IDENTIFY_CNS_NS_DESC_LIST 0x03	 /* Namespace Identification Descriptor list */
#define NVME_IDENTIFY_CNS_CS_NS 0x05		 /* I/O Command Set specific Identify Namespace */
#define NVME_IDENTIFY_CNS_CS_CTRL 0x06		 /* I/O Command Set specific Identify Controller */
#define NVME_IDENTIFY_CNS_CS_ACTIVE_NS_LIST 0x07 /* Active NSID list for one command set */
#define NVME_IDENTIFY_CNS_NS_CS_INDEP 0x08	 /* Command Set Independent Identify Namespace */
#define NVME_IDENTIFY_CNS_IO_COMMAND_SET 0x1C	 /* I/O Command Set data structure (combinations) */

/* Identify CDW11[31:24] carries the Command Set Identifier for the CS-specific CNS values. */
#define NVME_IDENTIFY_CDW11_CSI(cdw11) ((uint8_t)(((cdw11) >> 24) & 0xFFu))

/* Feature identifier 0x19, I/O Command Set Profile. The host issues it on every device open; a
 * controller that rejects it never gets as far as I/O. */
#define NVME_FEAT_IO_CMD_SET_PROFILE 0x19

/* Namespace Identification Descriptor entries: NIDT, NIDL, rsvd[2], NID[NIDL]. We emit a single
 * NGUID, giving the namespace a stable 128-bit identity. */
#define NVME_NIDT_EUI64 0x01
#define NVME_NIDT_NGUID 0x02
#define NVME_NIDT_UUID 0x03
#define NVME_NIDT_CSI 0x04 /* Command Set Identifier */
#define NVME_NIDT_NGUID_LEN 16
#define NVME_NIDT_CSI_LEN 1
#define NVME_NS_DESC_HDR_LEN 4 /* NIDT(1) + NIDL(1) + reserved(2) */
#define NVME_CSI_NVM 0x00      /* NVM command set */
#define NVME_CSI_KV 0x01       /* Key Value command set */

/*
 * KV Identify Namespace (CNS 05h with CSI=KV) field byte offsets. Only NSZE, MXKL and MXVL are
 * populated; a zero MXVL would advertise a namespace that accepts no value at all.
 */
#define NVME_ID_KV_NS_NSZE 0   /* Namespace Size (u64) */
#define NVME_ID_KV_NS_MXKL 8   /* Maximum Key Length in bytes (u16) */
#define NVME_ID_KV_NS_MXVL 10  /* Maximum Value Length in bytes (u32) */
#define NVME_ID_KV_NS_NUSE 16  /* Namespace Utilization (u64) */
#define NVME_ID_KV_NS_NGUID 48 /* Namespace GUID (16 B) */

/* Command Set Independent Identify Namespace (CNS 08h) field offsets. Only NSTAT matters to us: bit
 * 0 (NRDY) is how a namespace declares itself ready for I/O in the multiple-command-set model. */
#define NVME_ID_NS_CS_INDEP_NSTAT 14
#define NVME_NSTAT_NRDY (1u << 0)

/* Identify data structures and the log pages we return are a single 4 KiB page. */
#define NVME_IDENTIFY_DATA_SIZE 4096

/* NVMe revision 1.4.0, encoded as in the VS register / Identify Controller VER field. */
#define NVME_VERSION_1_4_0 0x00010400u

/*
 * Identify Controller data structure field byte offsets (NVMe Base spec 1.4, Fig. "Identify
 * Controller Data Structure"). We populate only the subset a host needs to enumerate; everything
 * else stays zero in the (zeroed) output buffer.
 */
#define NVME_ID_CTRL_VID 0   /* PCI Vendor ID (u16) */
#define NVME_ID_CTRL_SSVID 2 /* PCI Subsystem Vendor ID (u16) */
#define NVME_ID_CTRL_SN 4    /* Serial Number (20 B ASCII, space-padded) */
#define NVME_ID_CTRL_SN_LEN 20
#define NVME_ID_CTRL_MN 24 /* Model Number (40 B ASCII, space-padded) */
#define NVME_ID_CTRL_MN_LEN 40
#define NVME_ID_CTRL_FR 64 /* Firmware Revision (8 B ASCII, space-padded) */
#define NVME_ID_CTRL_FR_LEN 8
#define NVME_ID_CTRL_MDTS 77	/* Maximum Data Transfer Size (2^MDTS * CAP.MPSMIN pages) */
#define NVME_ID_CTRL_CNTLID 78	/* Controller ID (u16) */
#define NVME_ID_CTRL_VER 80	/* Version (u32) */
#define NVME_ID_CTRL_OACS 256	/* Optional Admin Command Support (u16) */
#define NVME_ID_CTRL_ACL 258	/* Abort Command Limit (0-based) */
#define NVME_ID_CTRL_AERL 259	/* Async Event Request Limit (0-based) */
#define NVME_ID_CTRL_FRMW 260	/* Firmware Updates */
#define NVME_ID_CTRL_LPA 261	/* Log Page Attributes */
#define NVME_ID_CTRL_SQES 512	/* Submission Queue Entry Size (min/max nibbles) */
#define NVME_ID_CTRL_CQES 513	/* Completion Queue Entry Size (min/max nibbles) */
#define NVME_ID_CTRL_NN 516	/* Number of Namespaces (u32) */
#define NVME_ID_CTRL_ONCS 520	/* Optional NVM Command Support (u16) */
#define NVME_ID_CTRL_VWC 525	/* Volatile Write Cache */
#define NVME_ID_CTRL_SGLS 536	/* SGL Support (u32) - 0 = SGLs not supported */
#define NVME_ID_CTRL_SUBNQN 768 /* NVM Subsystem NVMe Qualified Name (256 B, null-terminated) */
#define NVME_ID_CTRL_SUBNQN_LEN 256

/* SQES/CQES encode log2(entry size) in both the min [3:0] and max [7:4] nibbles: 2^6 = 64 B SQE,
 * 2^4 = 16 B CQE. */
#define NVME_ID_CTRL_SQES_VAL 0x66
#define NVME_ID_CTRL_CQES_VAL 0x44

/* Identify Namespace data structure field byte offsets. */
#define NVME_ID_NS_NSZE 0    /* Namespace Size in logical blocks (u64) */
#define NVME_ID_NS_NCAP 8    /* Namespace Capacity (u64) */
#define NVME_ID_NS_NUSE 16   /* Namespace Utilization (u64) */
#define NVME_ID_NS_NLBAF 25  /* Number of LBA Formats (0-based) */
#define NVME_ID_NS_FLBAS 26  /* Formatted LBA Size (selected format index) */
#define NVME_ID_NS_LBAF0 128 /* LBA Format 0 descriptor (u32) */
/* Namespace GUID (16 B). The host reads the namespace identity from CNS 00h at this offset, not
 * from the CNS 03h descriptor list, so it must be populated in both. */
#define NVME_ID_NS_NGUID 104
/* LBA Format: LBADS (log2 of LBA data size) in bits [23:16]. */
#define NVME_LBAF_LBADS_SHIFT 16

static inline void nvme_wr16(void *p, uint16_t v)
{
	uint16_t le = htole16(v);

	memcpy(p, &le, sizeof(le));
}

static inline void nvme_wr32(void *p, uint32_t v)
{
	uint32_t le = htole32(v);

	memcpy(p, &le, sizeof(le));
}

static inline void nvme_wr64(void *p, uint64_t v)
{
	uint64_t le = htole64(v);

	memcpy(p, &le, sizeof(le));
}

/* Write an NVMe ASCII identifier field: left-justified, space-padded, NOT null-terminated. */
static inline void nvme_wr_ascii(void *p, const char *s, size_t field_len)
{
	size_t n = strnlen(s, field_len);

	memcpy(p, s, n);
	memset((uint8_t *)p + n, ' ', field_len - n);
}

static inline uint16_t nvme_rd16(const void *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return le16toh(v);
}

static inline uint32_t nvme_rd32(const void *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return le32toh(v);
}

static inline uint64_t nvme_rd64(const void *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return le64toh(v);
}

/* Opcode: CDW0 bits [7:0] (byte 0). */
static inline uint8_t nvme_sqe_opcode(const void *sqe)
{
	return ((const uint8_t *)sqe)[0];
}

/* PSDT: CDW0 bits [15:14] (byte 1 bits [7:6]). */
static inline uint8_t nvme_sqe_psdt(const void *sqe)
{
	return (uint8_t)((((const uint8_t *)sqe)[1] >> 6) & 0x3);
}

/* Command Identifier: CDW0 bits [31:16] (bytes 2..3). */
static inline uint16_t nvme_sqe_cid(const void *sqe)
{
	return nvme_rd16((const uint8_t *)sqe + 2);
}

/* Namespace Identifier: CDW1. */
static inline uint32_t nvme_sqe_nsid(const void *sqe)
{
	return nvme_rd32((const uint8_t *)sqe + 4);
}

/* DPTR.PRP1: bytes 24..31. */
static inline uint64_t nvme_sqe_prp1(const void *sqe)
{
	return nvme_rd64((const uint8_t *)sqe + 24);
}

/* DPTR.PRP2: bytes 32..39. */
static inline uint64_t nvme_sqe_prp2(const void *sqe)
{
	return nvme_rd64((const uint8_t *)sqe + 32);
}

/* Command Dword n (n in 0..15), at byte offset n*4. */
static inline uint32_t nvme_sqe_cdw(const void *sqe, unsigned int n)
{
	return nvme_rd32((const uint8_t *)sqe + (size_t)n * 4);
}

/*****************************************************************************
 * KV command-set SQE accessors
 *****************************************************************************/

/* Longest key the KV command set can carry in the SQE dwords below. */
#define NVME_KV_MAX_KEY_LEN 16

/* Key length: CDW11[7:0]. */
static inline uint8_t nvme_kv_sqe_key_len(const void *sqe)
{
	return (uint8_t)(nvme_sqe_cdw(sqe, 11) & 0xFFu);
}

/* Value length in bytes: CDW10 (the spec's VS for Store, HBS for Retrieve). */
static inline uint32_t nvme_kv_sqe_value_len(const void *sqe)
{
	return nvme_sqe_cdw(sqe, 10);
}

/* Store options, CDW11: SIKE (store only if key exists), SINKE (store only if key does not). */
#define NVME_KV_SQE_STORE_SIKE (1u << 8)
#define NVME_KV_SQE_STORE_SINKE (1u << 9)

static inline uint32_t nvme_kv_sqe_store_opts(const void *sqe)
{
	return nvme_sqe_cdw(sqe, 11) & (NVME_KV_SQE_STORE_SIKE | NVME_KV_SQE_STORE_SINKE);
}

/*
 * Unpack the key into @out, zero-padded above key_len. The KV command set splits it across two
 * non-adjacent dword pairs: bytes 0..7 in CDW2/CDW3, bytes 8..15 in CDW14/CDW15.
 */
static inline void nvme_kv_sqe_get_key(const void *sqe, uint8_t out[NVME_KV_MAX_KEY_LEN])
{
	memcpy(out, (const uint8_t *)sqe + 8, 8);      /* CDW2, CDW3 */
	memcpy(out + 8, (const uint8_t *)sqe + 56, 8); /* CDW14, CDW15 */
}

#endif /* MEMOS_INITIATOR_REF_APP_NVME_SPEC_H_ */
