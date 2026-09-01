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

/**
 * @brief TLP side of the 2-app split.
 *
 * Two threads:
 *   - TLP core (main thread): runs doca_pe_progress in a tight loop.
 *     The PCI config change callback only snapshots state (seqlock-
 *     protected) and sets a pending flag — no syscalls, no blocking.
 *   - Management thread (tlp_mgmt_core): forwards pending state to
 *     EMU via IPC, monitors EMU child health (waitpid + respawn),
 *     handles LU trigger and LU messages.
 *
 * Live Update trigger:  touch /tmp/vblk_lu_trigger
 *   Mgmt thread spawns a DST EMU alongside the running SRC EMU.  Once the
 *   DST handover completes, SRC exits and TLP redirects state updates to DST.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libgen.h>
#include <linux/limits.h>
#include <unistd.h>
#include <bsd/string.h>

#include <doca_log.h>
#include <doca_error.h>
#include <doca_pe.h>
#include <doca_dev.h>

#include "vblk_pci_dev_core_lu.h"
#include "vblk_pci_lu.h"
#include "vblk_ctrl_lu.h"
#include "vblk_ipc.h"
#include "vblk_tlp_ctx_lu.h"
#include "vblk_ipc_msgs.h"
#include "vblk_handover.h"

DOCA_LOG_REGISTER(VBLK_TLP);

#define VBLK_TLP_MGMT_POLL_US 100000

enum vblk_emu_role {
	VBLK_EMU_SRC,
	VBLK_EMU_DST,
};

/* Recovery state: export descriptor + binary path, swapped atomically */
struct tlp_recovery {
	void *export_desc;
	size_t export_desc_len;
	char bin[PATH_MAX];
};

static struct tlp_recovery *tlp_recovery_create(void *export_desc, size_t export_desc_len, const char *bin)
{
	struct tlp_recovery *rec = calloc(1, sizeof(*rec));
	if (rec == NULL)
		return NULL;
	rec->export_desc = export_desc;
	rec->export_desc_len = export_desc_len;
	if (bin)
		snprintf(rec->bin, sizeof(rec->bin), "%s", bin);
	return rec;
}

static void tlp_recovery_destroy(struct tlp_recovery *rec)
{
	if (rec == NULL)
		return;
	vblk_export_desc_free(&rec->export_desc, &rec->export_desc_len);
	free(rec);
}

/* ─── TLP state ──────────────────────────────────────────────────── */

struct tlp_state {
	struct vblk_ipc_ep ipc;
	struct vblk_pci_virtio_dev *pci_dev;
	struct vblk_lu_device_cfg last_state;
	_Atomic(struct tlp_recovery *) recovery;

	/* Seqlock-protected: written by TLP core, read by mgmt thread */
	_Atomic uint32_t state_seq;
	_Atomic bool state_pending;

	pid_t src_pid;
	pid_t dst_pid;
	bool lu_in_progress;
	bool lu_awaiting_initiate_ack;
	struct vblk_pci_dev_config *config;

	/* DST EMU binary override, captured once at startup from VBLK_DST_EMU_BIN */
	const char *dst_emu_bin;
};

static struct tlp_state g_tlp;

static void tlp_state_reset(struct vblk_pci_dev_config *config)
{
	memset(&g_tlp, 0, sizeof(g_tlp));
	g_tlp.ipc.fd = -1;
	g_tlp.config = config;
	g_tlp.dst_emu_bin = getenv("VBLK_DST_EMU_BIN");
}

/* ─── Snapshot + IPC callback (TLP core — must not block) ────────── */

static void tlp_snapshot_state(struct vblk_pci_virtio_dev *dev, struct vblk_lu_device_cfg *out)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = vblk_pci_virtio_get_pci_cfg(dev);
	const struct vblk_pci_virtq_pci_cfg *vqs = vblk_pci_virtio_get_virtq_pci_cfg(dev);

	memset(out, 0, sizeof(*out));
	out->magic_header = VBLK_EMU_STATE_MAGIC;
	out->device_status = pci_cfg->device_status;
	out->driver_features = dev->driver_features;
	out->vq_cfg.num_queues = pci_cfg->num_queues;
	for (uint16_t q = 0; q < pci_cfg->num_queues; q++)
		out->vq_cfg.vqs[q] = vqs[q];
	out->magic_footer = VBLK_EMU_STATE_MAGIC;
}

static void tlp_cfg_change_cb(struct vblk_pci_virtio_dev *dev, void *arg)
{
	struct tlp_state *st = arg;
	uint8_t cur_status = vblk_pci_virtio_get_pci_cfg(dev)->device_status;
	uint8_t prev_status = st->last_state.device_status;

	/* Only two edges matter to EMU: device reset and driver ready.
	 * Intermediate status bits (ACK, DRIVER, FEATURES_OK) are irrelevant.
	 * Filtering here guarantees the two events are far enough apart in
	 * time that the mgmt thread's 100 ms poll never coalesces them. */
	bool is_reset = (cur_status == 0 && prev_status != 0);
	bool is_driver_ok = (cur_status & VBLK_PCI_VIRTIO_DEVICE_STATUS_DRIVER_OK) &&
			    !(prev_status & VBLK_PCI_VIRTIO_DEVICE_STATUS_DRIVER_OK);

	if (!is_reset && !is_driver_ok)
		return;

	uint32_t seq = st->state_seq;
	atomic_store_explicit(&st->state_seq, seq + 1, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);

	tlp_snapshot_state(dev, &st->last_state);

	atomic_store_explicit(&st->state_seq, seq + 2, memory_order_release);
	atomic_store_explicit(&st->state_pending, true, memory_order_release);
}

/* ─── Spawn helpers ──────────────────────────────────────────────── */

static pid_t spawn_emu(struct vblk_pci_dev_config *cfg, enum vblk_emu_role role)
{
	char config_str[VBLK_EMU_CONFIG_BUF_LEN];
	int written = vblk_config_serialize(cfg, config_str, sizeof(config_str));
	if (written < 0 || (size_t)written >= sizeof(config_str)) {
		DOCA_LOG_ERR("TLP: serialized EMU config truncated (%d >= %zu); raise VBLK_EMU_CONFIG_BUF_LEN",
			     written,
			     sizeof(config_str));
		return -1;
	}
	if (setenv(VBLK_EMU_CONFIG_ENV, config_str, 1) != 0)
		return -1;

	char exe[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (len <= 0)
		return -1;
	exe[len] = '\0';
	char *dir = dirname(exe);
	char emu_bin[PATH_MAX];

	const char *override = NULL;
	if (role == VBLK_EMU_DST)
		override = g_tlp.dst_emu_bin;
	else if (g_tlp.recovery && g_tlp.recovery->bin[0] != '\0')
		override = g_tlp.recovery->bin;

	if (override)
		snprintf(emu_bin, sizeof(emu_bin), "%s", override);
	else
		snprintf(emu_bin, sizeof(emu_bin), "%s/doca_vblk_pci_dev_emu", dir);

	const char *role_str = (role == VBLK_EMU_DST) ? "dst" : "src";
	pid_t pid;
	char *argv[] = {emu_bin, (char *)role_str, NULL};

	/* Put the child in its own process group so terminal-delivered signals
	 * (e.g. Ctrl-C SIGINT) reach only TLP. TLP orchestrates EMU's lifecycle
	 * via explicit SIGTERM in tlp_emu_children_stop, which EMU handles
	 * gracefully (cleanup + SHM unlink). Crash simulation is still possible
	 * via 'kill -INT <emu_pid>' targeted at a specific PID. */
	posix_spawnattr_t attr;
	int rc = posix_spawnattr_init(&attr);
	if (rc != 0) {
		DOCA_LOG_ERR("posix_spawnattr_init EMU (%s) failed: %s", role_str, strerror(rc));
		return -1;
	}
	(void)posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
	(void)posix_spawnattr_setpgroup(&attr, 0);

	rc = posix_spawn(&pid, emu_bin, NULL, &attr, argv, environ);
	(void)posix_spawnattr_destroy(&attr);
	if (rc != 0) {
		DOCA_LOG_ERR("posix_spawn EMU (%s) failed: %s", role_str, strerror(rc));
		return -1;
	}
	DOCA_LOG_INFO("TLP: spawned EMU %s pid=%d (%s)", role_str, pid, emu_bin);
	return pid;
}

static pid_t respawn_src(struct vblk_pci_dev_config *config, struct tlp_state *st)
{
	vblk_ipc_ep_close(&st->ipc);
	doca_error_t err = vblk_ipc_ep_open(&st->ipc, VBLK_IPC_TLP_PATH, VBLK_IPC_EMU_SRC_PATH);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: failed to reinit IPC after EMU crash");
		return -1;
	}
	return spawn_emu(config, VBLK_EMU_SRC);
}

/* ─── LU trigger + control ───────────────────────────────────────── */

static bool check_lu_trigger(void)
{
	return unlink(VBLK_LU_TRIGGER_PATH) == 0;
}

static void start_live_update(struct vblk_pci_dev_config *config, struct tlp_state *st)
{
	DOCA_LOG_INFO("TLP: *** Live Update triggered — spawning DST EMU ***");
	st->dst_pid = spawn_emu(config, VBLK_EMU_DST);
	if (st->dst_pid < 0) {
		DOCA_LOG_ERR("TLP: failed to spawn DST EMU");
		return;
	}
	st->lu_in_progress = true;
	st->lu_awaiting_initiate_ack = true;
}

static void tlp_poll_ipc_msgs(struct tlp_state *st)
{
	static uint8_t buf[VBLK_IPC_MAX_PAYLOAD];
	uint32_t type, len;
	struct vblk_msg_ho_device ho_dev;

	while (vblk_ipc_recv(&st->ipc, &type, buf, sizeof(buf), &len) == DOCA_SUCCESS) {
		switch (type) {
		case VBLK_MSG_STATE_REQUEST: {
			DOCA_LOG_INFO("TLP: EMU requested state — sending status=0x%x", st->last_state.device_status);
			/* Recovery state must arrive before state update:
			 * emu_request_initial_state() returns on STATE_UPDATE,
			 * so RECOVERY_STATE would be lost if sent after. */
			struct tlp_recovery *rec = st->recovery;
			vblk_ipc_send(&st->ipc,
				      VBLK_MSG_RECOVERY_STATE,
				      rec ? rec->export_desc : NULL,
				      rec ? rec->export_desc_len : 0);
			vblk_ipc_send(&st->ipc, VBLK_MSG_STATE_UPDATE, &st->last_state, sizeof(st->last_state));
			break;
		}
		case VBLK_MSG_EXPORT_DESC: {
			void *new_desc = NULL;
			size_t new_len = 0;

			doca_error_t store_err = vblk_export_desc_store(&new_desc, &new_len, buf, len);
			if (store_err != DOCA_SUCCESS) {
				DOCA_LOG_ERR("TLP: failed to store export descriptor: %s — keeping old",
					     doca_error_get_name(store_err));
				break;
			}

			const char *bin = st->recovery ? st->recovery->bin : NULL;

			struct tlp_recovery *rec = tlp_recovery_create(new_desc, new_len, bin);
			if (rec == NULL) {
				DOCA_LOG_ERR("TLP: failed to allocate recovery state");
				vblk_export_desc_free(&new_desc, &new_len);
				break;
			}

			/* Pointer swap — old descriptor released after new is live */
			struct tlp_recovery *old = st->recovery;
			st->recovery = rec;
			tlp_recovery_destroy(old);
			DOCA_LOG_INFO("TLP: received export descriptor (%u bytes)", len);
			break;
		}
		case VBLK_MSG_HO_INITIATE_ACK:
			if (!st->lu_awaiting_initiate_ack)
				break;
			st->lu_awaiting_initiate_ack = false;
			DOCA_LOG_INFO("TLP: DST ready — sending HANDOVER_DEVICE for device 0");
			memset(&ho_dev, 0, sizeof(ho_dev));
			ho_dev.device_id = 0;
			vblk_ipc_send_to(&st->ipc, VBLK_IPC_EMU_DST_PATH, VBLK_MSG_HO_DEVICE, &ho_dev, sizeof(ho_dev));
			break;
		case VBLK_MSG_HO_DEVICE_ACK:
			DOCA_LOG_INFO("TLP: device handover complete");
			break;
		case VBLK_MSG_HO_DEVICE_NACK:
			DOCA_LOG_WARN("TLP: device handover failed");
			st->lu_in_progress = false;
			break;
		default:
			break;
		}
	}
}

static void finalize_live_update(struct tlp_state *st)
{
	DOCA_LOG_INFO("TLP: SRC exited — DST has rebound to SRC path, redirecting");
	vblk_ipc_ep_set_peer(&st->ipc, VBLK_IPC_EMU_SRC_PATH);
	st->src_pid = st->dst_pid;
	st->dst_pid = -1;
	st->lu_in_progress = false;

	if (st->dst_emu_bin && st->recovery)
		snprintf(st->recovery->bin, sizeof(st->recovery->bin), "%s", st->dst_emu_bin);
}

/* ─── Management thread (tlp_mgmt_core) ──────────────────────────── */

static bool mgmt_read_state(struct tlp_state *st, struct vblk_emu_state *out)
{
	if (!atomic_load_explicit(&st->state_pending, memory_order_acquire))
		return false;

	uint32_t s1, s2 = 0;
	do {
		s1 = atomic_load_explicit(&st->state_seq, memory_order_acquire);
		if (s1 & 1)
			continue;
		memcpy(out, &st->last_state, sizeof(*out));
		atomic_thread_fence(memory_order_acquire);
		s2 = atomic_load_explicit(&st->state_seq, memory_order_relaxed);
	} while (s1 != s2);

	atomic_store_explicit(&st->state_pending, false, memory_order_relaxed);
	return true;
}

static void mgmt_check_stdin(struct tlp_state *st)
{
	(void)st;
	static char buf[128];
	static int pos;

	ssize_t n = read(STDIN_FILENO, buf + pos, sizeof(buf) - pos - 1);
	if (n <= 0)
		return;
	pos += n;
	buf[pos] = '\0';
	char *nl = strchr(buf, '\n');
	if (!nl)
		return;
	*nl = '\0';

	char command[32];
	unsigned long gb;
	if (sscanf(buf, "%31s %lu", command, &gb) == 2 && strcmp(command, "cap") == 0 && gb > 0) {
		uint64_t bytes = gb * 1024UL * 1024UL * 1024UL;
		vblk_pci_set_capacity(bytes);
		vblk_pci_notify_host();
		DOCA_LOG_INFO("MGMT: capacity set to %lu GB", gb);
	} else {
		DOCA_LOG_ERR("Invalid command. Usage: cap <capacity_in_gb>");
	}

	int remaining = pos - (int)(nl + 1 - buf);
	if (remaining > 0)
		memmove(buf, nl + 1, remaining);
	pos = remaining;
}

static void mgmt_check_src_child(struct tlp_state *st)
{
	if (st->src_pid <= 0)
		return;

	int wstatus;
	pid_t rc = waitpid(st->src_pid, &wstatus, WNOHANG);

	if (rc == 0)
		return;
	if (rc < 0) {
		DOCA_LOG_ERR("TLP: waitpid(%d) failed: %s", st->src_pid, strerror(errno));
		return;
	}

	if (st->lu_in_progress) {
		DOCA_LOG_INFO("TLP: SRC pid=%d exited during LU (expected)", st->src_pid);
		finalize_live_update(st);
		return;
	}

	if (WIFEXITED(wstatus))
		DOCA_LOG_WARN("TLP: SRC pid=%d exited (code=%d), respawning", st->src_pid, WEXITSTATUS(wstatus));
	else if (WIFSIGNALED(wstatus))
		DOCA_LOG_WARN("TLP: SRC pid=%d killed by signal %d, respawning", st->src_pid, WTERMSIG(wstatus));
	else
		return;

	st->src_pid = respawn_src(st->config, st);
	if (st->src_pid < 0)
		DOCA_LOG_ERR("TLP: failed to respawn SRC");
}

static void mgmt_check_dst_child(struct tlp_state *st)
{
	if (st->dst_pid <= 0)
		return;

	int wstatus;
	pid_t rc = waitpid(st->dst_pid, &wstatus, WNOHANG);
	if (rc > 0) {
		DOCA_LOG_WARN("TLP: DST pid=%d exited unexpectedly during LU", st->dst_pid);
		st->dst_pid = -1;
		st->lu_in_progress = false;
	}
}

static void *mgmt_thread_func(void *arg)
{
	struct tlp_state *st = arg;

	while (!force_quit) {
		struct vblk_lu_device_cfg snapshot;
		if (mgmt_read_state(st, &snapshot)) {
			DOCA_LOG_DBG("TLP mgmt: forwarding state status=0x%x", snapshot.device_status);
			vblk_ipc_send(&st->ipc, VBLK_MSG_STATE_UPDATE, &snapshot, sizeof(snapshot));
		}

		mgmt_check_src_child(st);
		mgmt_check_dst_child(st);
		mgmt_check_stdin(st);
		tlp_poll_ipc_msgs(st);

		if (!st->lu_in_progress && check_lu_trigger())
			start_live_update(st->config, st);

		usleep(VBLK_TLP_MGMT_POLL_US);
	}

	return NULL;
}

static doca_error_t mgmt_thread_start(pthread_t *thread, uint8_t core, void *arg)
{
	pthread_attr_t attr;
	cpu_set_t cpus;
	int rc;

	if (pthread_attr_init(&attr) != 0)
		return DOCA_ERROR_INITIALIZATION;
	CPU_ZERO(&cpus);
	CPU_SET(core, &cpus);
	if (pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus) != 0) {
		pthread_attr_destroy(&attr);
		return DOCA_ERROR_INITIALIZATION;
	}
	rc = pthread_create(thread, &attr, mgmt_thread_func, arg);
	pthread_attr_destroy(&attr);
	return (rc == 0) ? DOCA_SUCCESS : DOCA_ERROR_INITIALIZATION;
}

/* ─── Init / cleanup helpers ─────────────────────────────────────── */

static doca_error_t tlp_pci_endpoint_create(struct vblk_pci_dev_config *config)
{
	const struct vblk_pci_virtio_blk_config blk_cfg = {
		.num_queues = config->num_queues,
		.seg_max = config->seg_max ? config->seg_max : 1,
		.size_max = 4096,
		.capacity = VBLK_CTRL_CAPACITY_BYTES / 512,
	};
	struct vblk_pci_virtio_attrs pci_attr = {
		.num_queues = config->num_queues,
		.device_type = VBLK_PCI_DEVICE_TYPE_VBLK,
		.device_features = (1 << VBLK_F_MQ) | (1 << VBLK_PCI_VIRTIO_F_SEG_MAX) |
				   (1 << VBLK_PCI_VIRTIO_F_SIZE_MAX),
		.pci_cfg_change_cb = tlp_cfg_change_cb,
		.dev_cfg = &blk_cfg,
		.cb_arg = &g_tlp,
	};
	if (config->indirect_enabled)
		pci_attr.device_features |= (1ULL << VBLK_PCI_VIRTIO_F_INDIRECT_DESC);

	tlp_state_reset(config);
	g_tlp.dst_pid = -1;
	g_tlp.pci_dev = vblk_pci_virtio_dev_create(&pci_attr);
	return g_tlp.pci_dev ? DOCA_SUCCESS : DOCA_ERROR_INITIALIZATION;
}

static doca_error_t tlp_emu_child_start(struct vblk_pci_dev_config *config)
{
	doca_error_t err = vblk_ipc_ep_open(&g_tlp.ipc, VBLK_IPC_TLP_PATH, VBLK_IPC_EMU_SRC_PATH);
	if (err != DOCA_SUCCESS)
		return err;

	unlink(VBLK_LU_TRIGGER_PATH);
	g_tlp.src_pid = spawn_emu(config, VBLK_EMU_SRC);
	if (g_tlp.src_pid < 0) {
		vblk_ipc_ep_close(&g_tlp.ipc);
		return DOCA_ERROR_INITIALIZATION;
	}
	return DOCA_SUCCESS;
}

static void tlp_emu_children_stop(void)
{
	int wstatus;

	if (g_tlp.src_pid > 0) {
		kill(g_tlp.src_pid, SIGTERM);
		(void)waitpid(g_tlp.src_pid, &wstatus, 0);
	}
	if (g_tlp.dst_pid > 0) {
		kill(g_tlp.dst_pid, SIGTERM);
		(void)waitpid(g_tlp.dst_pid, &wstatus, 0);
	}
	tlp_recovery_destroy(g_tlp.recovery);
	g_tlp.recovery = NULL;
	vblk_ipc_ep_close(&g_tlp.ipc);

	unlink(VBLK_IPC_EMU_SRC_PATH);
	unlink(VBLK_IPC_EMU_DST_PATH);
}

/* ─── TLP run (called from vblk_pci_dev.c main) ─────────────────── */

doca_error_t vblk_pci_dev_tlp_run(struct vblk_pci_dev_config *config)
{
	struct doca_dev *dev = NULL;
	struct doca_pe *pe = NULL;
	pthread_t tlp_thread, mgmt_thread;
	bool tlp_started = false, mgmt_started = false;
	doca_error_t err;

	(void)fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

	if (config->tlp_mgmt_core_idx == config->tlp_core_idx) {
		DOCA_LOG_ERR("tlp_mgmt_core_idx (%u) must not equal tlp_core_idx (%u)",
			     config->tlp_mgmt_core_idx,
			     config->tlp_core_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	struct vblk_app_cfg app_cfg = {0};
	strlcpy(app_cfg.device_name, config->device_name, sizeof(app_cfg.device_name));
	app_cfg.indirect_enabled = config->indirect_enabled;
	app_cfg.datapath_on_dpa = config->datapath_on_dpa;
	err = vblk_init(&app_cfg, &dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: vblk_init failed: %s", doca_error_get_name(err));
		return err;
	}

	err = vblk_pci_init(dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: vblk_pci_init failed: %s", doca_error_get_name(err));
		goto cleanup;
	}

	err = tlp_pci_endpoint_create(config);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: PCI endpoint create failed: %s", doca_error_get_name(err));
		goto cleanup;
	}

	err = tlp_emu_child_start(config);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: EMU child start failed: %s", doca_error_get_name(err));
		goto cleanup;
	}

	err = mgmt_thread_start(&mgmt_thread, config->tlp_mgmt_core_idx, &g_tlp);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: management thread create failed: %s", doca_error_get_name(err));
		goto cleanup;
	}
	mgmt_started = true;
	DOCA_LOG_INFO("TLP: management thread on core %u", config->tlp_mgmt_core_idx);

	err = doca_pe_create(&pe);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: PE create failed: %s", doca_error_get_name(err));
		goto stop_threads;
	}

	err = vblk_pci_tlp_thread_create(pe, &tlp_thread, config->tlp_core_idx);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("TLP: TLP thread create failed: %s", doca_error_get_name(err));
		goto stop_threads;
	}
	tlp_started = true;

	DOCA_LOG_INFO("TLP: running on core %u", config->tlp_core_idx);
	DOCA_LOG_INFO("Runtime commands: cap <GB> (e.g. 'cap 1' to set capacity to 1 GB)");
	goto join_threads;

stop_threads:
	force_quit = true;
join_threads:
	if (tlp_started)
		pthread_join(tlp_thread, NULL);
	if (mgmt_started)
		pthread_join(mgmt_thread, NULL);

cleanup:
	tlp_emu_children_stop();
	if (g_tlp.pci_dev)
		vblk_pci_virtio_dev_destroy(g_tlp.pci_dev);
	vblk_pci_reset();
	vblk_reset();
	if (dev)
		doca_dev_close(dev);
	if (pe)
		doca_pe_destroy(pe);
	unlink(VBLK_LU_TRIGGER_PATH);
	return err;
}
