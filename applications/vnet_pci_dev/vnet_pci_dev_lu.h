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

#ifndef VNET_PCI_DEV_LU_H_
#define VNET_PCI_DEV_LU_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <linux/if_ether.h>

#include <doca_error.h>

#include "vnet_pci_device.h" /* VNET_TOTAL_VQS, VNET_CTRL_MAX_QUEUES_PAIRS */

/* Forward declarations */
struct doca_devemu_vnet_offload_engine;
struct doca_devemu_pci_type;
struct vnet_pci_dev_resources;
struct vnet_pci_dev_config;
struct doca_dev_rep;
struct tlp_context;
struct doca_dev;

/**
 * @brief Live update mode -- process role in the handover protocol
 */
enum vnet_lu_mode {
	VNET_LU_MODE_NONE = 0, /* Normal operation */
	VNET_LU_MODE_ACTIVE,   /* Current traffic owner */
	VNET_LU_MODE_STANDBY,  /* Restores from active, then becomes active for chainable LU */
};

/** Check if live update is enabled (active or standby) */
static inline bool vnet_lu_is_enabled(enum vnet_lu_mode mode)
{
	return mode == VNET_LU_MODE_ACTIVE || mode == VNET_LU_MODE_STANDBY;
}

/** Check if this mode starts as standby (restore from active) */
static inline bool vnet_lu_is_standby(enum vnet_lu_mode mode)
{
	return mode == VNET_LU_MODE_STANDBY;
}

/** Unix domain socket path for handover coordination */
#define VNET_LU_SOCK_PATH "/tmp/vnet_live_update.sock"

/** Lock file to ensure only one standby at a time */
#define VNET_LU_LOCK_PATH "/tmp/vnet_live_update.lock"

/** Active accept() timeout (seconds) */
#define VNET_LU_ACCEPT_TIMEOUT_SEC 300

/** Active ACK timeout (seconds) */
#define VNET_LU_ACK_TIMEOUT_SEC 300

/** Standby idle timeout (seconds) */
#define VNET_LU_STANDBY_IDLE_TIMEOUT_SEC 300

/** Active ready-signal timeout (seconds) */
#define VNET_LU_READY_TIMEOUT_SEC 300

/** SCM_RIGHTS marker byte (not part of the enum-based protocol) */
#define VNET_LU_MSG_CMD_FD 'F'

/** Protocol messages exchanged over the UDS connection.
 *  All messages are sent as a uint32_t.
 *  Device LU and Channel LU share the same socket sequentially. */
enum vnet_lu_msg {
	VNET_LU_MSG_INVALID = 0,
	/* Device LU */
	VNET_LU_MSG_DEV_READY, /* standby pre-copy init complete */
	VNET_LU_MSG_DEV_GO,    /* per-device switchover trigger */
	VNET_LU_MSG_DEV_ACK,   /* all devices enabled */
	/* Channel LU */
	VNET_LU_MSG_CH_EXPORT,	  /* active -> standby: export_desc */
	VNET_LU_MSG_CH_BEGIN,	  /* standby -> active: ready for switchover */
	VNET_LU_MSG_CH_BEGIN_ACK, /* active -> standby: config state */
	VNET_LU_MSG_CH_END,	  /* standby -> active: outcome */
	VNET_LU_MSG_CH_END_ACK,	  /* active -> standby: ack */
	VNET_LU_MSG_NACK,	  /* error response (no payload) */
};

/*********************************************************************************************************************
 * Shared memory data structures for live update state transfer
 *********************************************************************************************************************/

/** Shared memory region name for POSIX shm_open */
#define VNET_LU_SHM_NAME "/vnet_live_update"

/** Shared memory directory for TLP channel export */
#define VNET_LU_CH_SHM_DIR "/dev/shm/vnet_lu_channel"

/** Channel LU operation timeout (seconds) */
#define VNET_LU_CH_TIMEOUT_SEC 60

/** Maximum allowed export descriptor size (sanity bound for malloc) */
#define VNET_LU_CH_EXPORT_MAX_LEN (1024 * 1024)

/** Magic number for shared memory validation ("VNET") */
#define VNET_LU_MAGIC 0x564E4554

/**
 * @brief Live update state machine
 *
 * Tracked in shared memory, queryable by operator tools.
 */
enum vnet_lu_state {
	VNET_LU_STATE_INITIALIZED = 0, /* App started, ready for live update */
	VNET_LU_STATE_PRE_COPY,	       /* Saving state to SHM */
	VNET_LU_STATE_STOPPED,	       /* Engine torn down, traffic stopped */
	VNET_LU_STATE_MIGRATE_REQUEST, /* Reconstructing device (standby) */
	VNET_LU_STATE_MIGRATED,	       /* Serving traffic, handover complete */
};

struct vnet_lu_vq_state {
	uint16_t index;
	uint16_t size;
	uint16_t msix_vector;
	uint64_t desc_addr;
	uint64_t driver_addr;
	uint64_t device_addr;
	uint8_t enabled;
};

struct vnet_lu_net_config {
	uint8_t mac[ETH_ALEN];
	uint16_t mtu;
};

struct vnet_lu_virtio_config {
	uint64_t device_features;
	uint16_t max_queue_pairs;
	uint16_t num_active_qps;
	uint16_t queue_size;
	bool mq_feature_negotiated;
};

struct vnet_lu_vqs_state {
	uint16_t num_vqs;
	struct vnet_lu_vq_state vqs[VNET_TOTAL_VQS(VNET_CTRL_MAX_QUEUES_PAIRS)];
};

struct vnet_lu_device_state {
	uint32_t magic;
	uint16_t ep_vhca_id; /* endpoint representor vhca_id (key for reattach) */
	struct vnet_lu_net_config net;
	struct vnet_lu_virtio_config virtio;
	struct vnet_lu_vqs_state vqs;
	uint32_t blob_offset; /* byte offset from SHM start to export blob */
	uint32_t blob_len;    /* export blob length in bytes */
};

struct vnet_lu_shm {
	enum vnet_lu_state state;
	uint32_t num_devices;
	uint32_t blob_data_offset; /* byte offset from SHM start to first blob */
	struct vnet_lu_device_state devices[];
};

/*********************************************************************************************************************
 * Channel LU -- per-device TLP config state transferred during channel switchover
 *********************************************************************************************************************/

struct vnet_lu_channel_dev_state {
	/* PCI config space header (type0 for endpoints, type1 for bridges) */
	union {
		struct pci_cfg_type0_header type0;
		struct pci_cfg_type1_header type1;
	} cfg_space_hdr;

	/* PCI capabilities (express, MSI, MSI-X, VPD, PM) */
	struct pci_capabilities caps;

	/* Device topology (set from first TLP) */
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint8_t is_bdf_set;
	uint16_t bdf;

	/* TLP transaction fields (same layout as struct pci_device_config / TLP handler sample) */
	uint16_t requester_id;
	uint16_t completer_id;
	uint16_t tag9 : 1;
	uint16_t tag8 : 1;
	uint16_t tag : 8;
	uint8_t req_fmt;
	uint8_t req_type;
	uint8_t cmpl_fmt;
	uint8_t cmpl_type;
	uint8_t cmpl_status;
	uint16_t cmpl_length;
	uint32_t cmpl_data;
};

struct vnet_lu_channel_config {
	uint32_t num_devices;
	uint32_t num_bridges;
	uint32_t num_ep;
	uint32_t transaction_region_size;
	struct vnet_lu_channel_dev_state devices[MAX_NUM_BRIDGE + MAX_NUM_EP];
};

/*********************************************************************************************************************
 * Public API
 *********************************************************************************************************************/

/** @brief Init active mode: register SIGUSR1 handler + create listen socket */
doca_error_t vnet_lu_active_init(void);

/** @brief Post-loop: run handover if SIGUSR1 was received, then cleanup */
bool vnet_lu_active_post_loop(struct vnet_pci_dev_resources *resources);

/** @brief Export all device state and offload engine blobs to SHM */
doca_error_t vnet_lu_save_state(struct tlp_context *tlp_ctx);

/** @brief Send a file descriptor over a Unix socket via SCM_RIGHTS */
doca_error_t vnet_lu_send_cmd_fd(int sock_fd, int fd_to_send);

/** @brief Receive a file descriptor from a Unix socket via SCM_RIGHTS */
doca_error_t vnet_lu_recv_cmd_fd(int sock_fd, int *fd_received);

/** @brief Extract the kernel cmd_fd from a doca_dev (via its ibv_pd) */
doca_error_t vnet_lu_get_cmd_fd(struct doca_dev *dev, int *cmd_fd_out);

/** @brief Standby: connect to active, receive cmd_fd, open SHM, reconstruct doca_dev */
doca_error_t vnet_lu_restore_early(struct vnet_pci_dev_resources *resources);

/** @brief Return the mapped SHM pointer (valid between restore_early and release_shm) */
const struct vnet_lu_shm *vnet_lu_get_shm(void);

/** @brief Unmap and release the standby-side SHM */
void vnet_lu_release_shm(void);

/** @brief Get the export blob for device @idx from SHM */
doca_error_t vnet_lu_get_import_desc(uint32_t idx, const void **export_desc, size_t *export_desc_len);

/** @brief Get the saved endpoint vhca_id for device @idx (used for rep reattach) */
doca_error_t vnet_lu_get_ep_vhca_id(uint32_t idx, uint16_t *vhca_id_out);

/*********************************************************************************************************************
 * Two-phase handover protocol
 *   Phase 1 (pre-copy):   cmd_fd --> init --> DEV_READY          [traffic flowing]
 *   Phase 2 (switchover): DEV_GO x N --> parallel enable --> DEV_ACK  [traffic down]
 *********************************************************************************************************************/

/** @brief Phase 1: standby signals active that pre-copy init is complete (DEV_READY) */
doca_error_t vnet_lu_phase1_send_ready(void);

/** @brief Phase 2: send final DEV_ACK to active and unlink SHM */
doca_error_t vnet_lu_restore_complete(void);

/** @brief Clean up standby-side resources (connection fd, etc.) */
void vnet_lu_close_conn(void);

/** @brief Check whether a LU handover was triggered (SIGUSR1 received) */
bool vnet_lu_handover_was_triggered(void);

/** @brief Query the persistent SF vhca_id from an offload engine's representor */
doca_error_t vnet_lu_get_sf_vhca_id(struct doca_devemu_vnet_offload_engine *engine, uint16_t *vhca_id_out);

/** @brief Find and open an existing representor by vhca_id (for rep reattach) */
doca_error_t vnet_lu_find_existing_rep(struct doca_devemu_pci_type *pci_type,
				       uint16_t target_vhca_id,
				       struct doca_dev_rep **rep_out);

/** @brief Override app config with SHM-restored values after restore_early */
void vnet_lu_override_config(struct vnet_pci_dev_config *config, const struct tlp_context *tlp_ctx, uint8_t *mac_bytes);

/** @brief Phase 1: Validate SHM, override num_ep, replay VQs (start_vqs, no enable), release SHM */
doca_error_t vnet_lu_apply_shm_replay(struct vnet_pci_dev_resources *resources, struct vnet_pci_dev_config *config);

/** @brief Phase 2: Parallel per-device enable -- spawns a thread per 'G' received, joins all */
doca_error_t vnet_lu_phase2_enable_engines(struct vnet_pci_dev_resources *resources);

/** @brief Replay saved device state from SHM (VQ addresses, features, OE start) */
doca_error_t vnet_pci_dev_lu_replay(struct vnet_pci_dev_resources *resources,
				    const struct vnet_lu_device_state *dev_states,
				    uint32_t num_devices);

/*********************************************************************************************************************
 * Channel LU -- TLP channel handover after device LU
 *   EXPORT -> BEGIN -> END (SETUP phase done by device LU: socket + cmd_fd)
 *********************************************************************************************************************/

/** @brief Active: create SHM dir, set shm_dir_path, and mark channel as primary for export */
doca_error_t vnet_lu_channel_enable_export(struct doca_devemu_pci_tlp_channel *tlp_channel);

/** @brief Standby: receive export, create secondary channel, apply config, become primary */
doca_error_t vnet_lu_channel_restore(struct vnet_pci_dev_resources *resources);

#endif /* VNET_PCI_DEV_LU_H_ */
