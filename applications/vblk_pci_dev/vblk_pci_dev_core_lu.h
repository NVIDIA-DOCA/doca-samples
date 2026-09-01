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

#ifndef VBLK_PCI_DEV_CORE_LU_H
#define VBLK_PCI_DEV_CORE_LU_H

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_pe.h>

#include "vblk_ctrl_lu.h"

/* Default configuration values */
#define VBLK_PCI_DEV_DEFAULT_TLP_CORE_IDX 0
#define VBLK_PCI_DEV_DEFAULT_TLP_MGMT_CORE_IDX 15
#define VBLK_PCI_DEV_DEFAULT_OFFLOAD_ENGINE_CORE_IDX 1
#define VBLK_PCI_DEV_DEFAULT_IO_CTX_MASK 0xFFFEUL /* cores 1-15 (15 cores total including offload engine) */
#define VBLK_PCI_DEV_DEFAULT_NUM_QUEUES 255
#define VBLK_PCI_DEV_DEFAULT_SHM_DIR_PATH "/dev/shm"

/* Buffer size for the shm_dir_path field; matches the library cap
 * PRIV_DOCA_DEVEMU_VIRTIO_MAX_SHM_DIR_PATH_LEN. */
#define VBLK_SHM_DIR_PATH_LEN 256

/* Global force quit flag (defined in vblk_pci_dev.c) */
extern volatile bool force_quit;

/**
 * @brief Progress engine context for application threads
 *
 * Each IO context thread and the TLP thread has one PE context, containing
 * the DOCA progress engine, the thread handle, and initialization state.
 */
struct vblk_pci_dev_pe_context {
	struct doca_pe *pe; /* DOCA progress engine handle */
	pthread_t thread;   /* Thread handle for this PE context */
	bool initialized;   /* True when PE is created and ready */
};

/**
 * @brief Application resources structure
 *
 * Contains all runtime resources for the VirtIO Block PCI device application.
 * This structure aggregates device handles, PE contexts, and the block controller.
 * Mirrors vnet_pci_dev_resources for consistent architecture across VirtIO devices.
 */
struct vblk_pci_dev_resources {
	struct vblk_pci_dev_pe_context *io_pe_ctxs; /* Array of IO PE contexts (num_io_ctx) */
	struct vblk_ctrl vblk_ctrl;		    /* Multi-device: becomes an array of controllers */
	uint8_t num_io_ctx;			    /* Number of IO contexts */
};

/**
 * @brief Application configuration structure
 *
 * Contains all configuration parameters for the VirtIO Block PCI device application.
 * This structure is populated from command-line arguments and provides immutable
 * configuration throughout the application lifecycle.
 */
struct vblk_pci_dev_config {
	char device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* Emulation manager device name */
	uint16_t num_queues;				/* Number of virtio queues (1-255) */
	uint16_t seg_max;				/* Max segments per request (0 = default 1) */
	uint64_t io_ctx_mask;				/* IO contexts CPU mask */
	uint8_t tlp_core_idx;				/* TLP core index (not in io_ctx_mask) */
	uint8_t tlp_mgmt_core_idx;			/* TLP management thread core (must not equal tlp_core_idx) */
	uint8_t offload_engine_core_idx;		/* Offload engine core (in io_ctx_mask) */
	bool datapath_on_dpa;				/* Data path provider */
	uint32_t stats_ios_period;			/* Stats query period (0 = disabled) */
	bool indirect_enabled;				/* Enable indirect descriptor feature */
	char shm_dir_path[VBLK_SHM_DIR_PATH_LEN];	/* Directory for SHM files (live-update + recovery) */
};

/* Env var used to pass config from TLP parent to EMU child process.
 *
 * Serialized fields (9 total — keep serialize/deserialize/NFIELDS in sync):
 *   device_name, num_queues, seg_max, indirect_enabled,
 *   offload_engine_core_idx, io_ctx_mask, stats_ios_period,
 *   datapath_on_dpa, shm_dir_path
 *
 * Fields NOT serialized (TLP-only, EMU doesn't need them):
 *   tlp_core_idx, tlp_mgmt_core_idx
 */
#define VBLK_EMU_CONFIG_ENV "VBLK_EMU_CONFIG"
#define VBLK_EMU_CONFIG_NFIELDS 9

/* Numeric literals used as sscanf widths in vblk_config_deserialize(); sscanf
 * requires a literal width in the format string, so these cannot be expressed
 * as (BUF_SIZE - 1). The static asserts below pin them to the destination
 * buffer sizes so a change to DOCA_DEVINFO_IBDEV_NAME_SIZE or
 * VBLK_SHM_DIR_PATH_LEN breaks the build here and forces the literal to be
 * updated in lockstep. */
#define VBLK_DEV_NAME_MAX_LEN 63 /* DOCA_DEVINFO_IBDEV_NAME_SIZE - 1 */
#define VBLK_SHM_DIR_MAX_LEN 255 /* VBLK_SHM_DIR_PATH_LEN - 1 */

_Static_assert(DOCA_DEVINFO_IBDEV_NAME_SIZE == VBLK_DEV_NAME_MAX_LEN + 1,
	       "VBLK_DEV_NAME_MAX_LEN must equal DOCA_DEVINFO_IBDEV_NAME_SIZE - 1; "
	       "sscanf width literal in vblk_config_deserialize() must be kept in sync with device_name[]");
_Static_assert(VBLK_SHM_DIR_PATH_LEN == VBLK_SHM_DIR_MAX_LEN + 1,
	       "VBLK_SHM_DIR_MAX_LEN must equal VBLK_SHM_DIR_PATH_LEN - 1; "
	       "sscanf width literal in vblk_config_deserialize() must be kept in sync with shm_dir_path[]");

/* Stringify MAX_LEN since sscanf needs a literal width in the format string. */
#define _VBLK_STR(s) #s
#define _VBLK_XSTR(s) _VBLK_STR(s)
#define VBLK_DEV_NAME_SCANF_W _VBLK_XSTR(VBLK_DEV_NAME_MAX_LEN)
#define VBLK_SHM_DIR_SCANF_W _VBLK_XSTR(VBLK_SHM_DIR_MAX_LEN)

/* Padded upper bound on separators + numeric/bool fields, with slack for future fields. */
#define VBLK_EMU_CONFIG_NUMERIC_OVERHEAD 96
#define VBLK_EMU_CONFIG_BUF_LEN (VBLK_DEV_NAME_MAX_LEN + VBLK_SHM_DIR_MAX_LEN + VBLK_EMU_CONFIG_NUMERIC_OVERHEAD)

static inline int vblk_config_serialize(const struct vblk_pci_dev_config *cfg, char *buf, size_t len)
{
	return snprintf(buf,
			len,
			"%s %hu %hu %d %hhu %" PRIu64 " %u %d %s",
			cfg->device_name,
			cfg->num_queues,
			cfg->seg_max,
			cfg->indirect_enabled ? 1 : 0,
			cfg->offload_engine_core_idx,
			cfg->io_ctx_mask,
			cfg->stats_ios_period,
			cfg->datapath_on_dpa ? 1 : 0,
			cfg->shm_dir_path);
}

/*
 * POSIX C-locale whitespace check without the libc ctype-table lookup.
 * isspace() on glibc indexes __ctype_b[] by the character value, which makes
 * Coverity treat any call on a tainted char (e.g. one read from the
 * VBLK_EMU_CONFIG env-var) as a TAINTED_SCALAR-as-offset use and report it
 * at the surrounding function's call sites. Used in both the env-var
 * deserializer and the shm_dir_path validator since both operate on
 * sscanf-populated data ultimately derived from that tainted env-var.
 */
static inline bool vblk_is_whitespace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static inline bool vblk_config_deserialize(const char *buf, struct vblk_pci_dev_config *cfg)
{
	int ind, dpa;
	int pos = 0;

	/* Trailing " %n" records the parse position so we can reject extra junk
	 * after the last field, including a silently truncated overlong
	 * shm_dir_path whose remainder would otherwise be discarded. */
	int n = sscanf(buf,
		       "%" VBLK_DEV_NAME_SCANF_W "s %hu %hu %d %hhu %" SCNu64 " %u %d %" VBLK_SHM_DIR_SCANF_W "s %n",
		       cfg->device_name,
		       &cfg->num_queues,
		       &cfg->seg_max,
		       &ind,
		       &cfg->offload_engine_core_idx,
		       &cfg->io_ctx_mask,
		       &cfg->stats_ios_period,
		       &dpa,
		       cfg->shm_dir_path,
		       &pos);
	if (n != VBLK_EMU_CONFIG_NFIELDS)
		return false;

	/* Bound pos against the caller's contract on buf before using it as an
	 * offset; sscanf reports the parse position back via %n, which Coverity
	 * tracks as tainted because it was derived from tainted input. */
	if (pos < 0 || pos >= VBLK_EMU_CONFIG_BUF_LEN)
		return false;

	while (vblk_is_whitespace(buf[pos]))
		pos++;
	if (buf[pos] != '\0')
		return false;

	cfg->indirect_enabled = (ind != 0);
	cfg->datapath_on_dpa = (dpa != 0);
	return true;
}

/*
 * Shared invariants for shm_dir_path on both the CLI and EMU sides. Silent on
 * failure so callers can use their own logging surface (DOCA_LOG_ERR from the
 * CLI argp callback; fprintf from the EMU child before doca_log_backend_create_standard()).
 * Whitespace is rejected via vblk_is_whitespace() (see its comment) to match
 * the EMU deserializer's notion of token separators while keeping Coverity happy.
 */
static inline doca_error_t vblk_validate_shm_dir_path(const char *path)
{
	size_t len;

	if (path == NULL || path[0] == '\0')
		return DOCA_ERROR_INVALID_VALUE;

	len = strnlen(path, VBLK_SHM_DIR_PATH_LEN);
	if (len >= VBLK_SHM_DIR_PATH_LEN)
		return DOCA_ERROR_INVALID_VALUE;

	if (path[0] != '/')
		return DOCA_ERROR_INVALID_VALUE;

	for (size_t i = 0; i < len; i++) {
		if (vblk_is_whitespace(path[i]))
			return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/**
 * @brief TLP app entry point — creates endpoint, spawns EMU child, runs TLP loop
 *
 * Multi-device: tlp_state and spawn/respawn logic must become per-device.
 * Each device needs its own IPC channel and PCI config callback.
 */
doca_error_t vblk_pci_dev_tlp_run(struct vblk_pci_dev_config *config);

/**
 * @brief EMU app run function
 *
 * Creates the offload engine and IO contexts, exports OE state (SRC)
 * or kicks off handover (DST), then runs the IPC poll loop until force_quit.
 *
 * @param[in] config  Application configuration
 * @param[in] ep      PCI endpoint for the offload engine
 * @param[in] ipc     IPC endpoint for receiving state updates / handover msgs
 * @param[in] role    VBLK_HO_ROLE_SRC or VBLK_HO_ROLE_DST
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
struct doca_devemu_pci_ep;
struct vblk_ipc_ep;
doca_error_t vblk_pci_dev_emu_run(struct vblk_pci_dev_config *config,
				  struct doca_devemu_pci_ep *ep,
				  struct vblk_ipc_ep *ipc,
				  int role);

#endif /* VBLK_PCI_DEV_CORE_LU_H */
