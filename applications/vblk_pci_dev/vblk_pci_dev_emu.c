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
 * @brief EMU child process — spawned by the TLP app.
 *
 * Supports two roles:
 *   "src" (default) — normal operation: receives state from TLP, runs VQs.
 *                     Also acts as handover SRC when a DST connects.
 *   "dst"           — live-update destination: connects to SRC, drives the
 *                     4-phase handover, takes over VQs.
 *
 * Usage: doca_vblk_pci_dev_emu <dev> <nq> <seg_max> <indirect> <oe_core> <io_mask> <stats> [src|dst]
 */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_devemu_pci_tlp.h>

#include "vblk_pci_dev_core_lu.h"
#include "vblk_pci_lu.h"
#include "vblk_ctrl_lu.h"
#include "vblk_ipc.h"
#include "vblk_ipc_msgs.h"
#include "vblk_handover.h"

DOCA_LOG_REGISTER(VBLK_EMU);

volatile bool force_quit = false;

static void signal_handler(int signum)
{
	if (signum == SIGTERM)
		force_quit = true;
}

int main(int argc, char **argv)
{
	const char *cfg_str = getenv(VBLK_EMU_CONFIG_ENV);
	if (!cfg_str) {
		fprintf(stderr, "EMU: %s env var not set (must be spawned by TLP app)\n", VBLK_EMU_CONFIG_ENV);
		return EXIT_FAILURE;
	}

	/* Bound the env-var (tainted input) against the known serialized-size cap
	 * before passing it to the deserializer; without this the parser's
	 * sscanf-%n-derived offset is operating on unbounded tainted data. */
	if (strnlen(cfg_str, VBLK_EMU_CONFIG_BUF_LEN) >= VBLK_EMU_CONFIG_BUF_LEN) {
		fprintf(stderr,
			"EMU: %s exceeds max serialized size %d (truncated or tampered)\n",
			VBLK_EMU_CONFIG_ENV,
			VBLK_EMU_CONFIG_BUF_LEN - 1);
		return EXIT_FAILURE;
	}

	struct vblk_pci_dev_config config = {0};
	if (!vblk_config_deserialize(cfg_str, &config)) {
		fprintf(stderr, "EMU: failed to parse %s\n", VBLK_EMU_CONFIG_ENV);
		return EXIT_FAILURE;
	}

	/* Same invariants the CLI enforced; revalidate here so a tampered or
	 * truncated env-var value cannot reach the offload engine setup path. */
	if (vblk_validate_shm_dir_path(config.shm_dir_path) != DOCA_SUCCESS) {
		fprintf(stderr,
			"EMU: invalid shm_dir_path '%s' in %s (must be non-empty, absolute, no whitespace, <=%d chars)\n",
			config.shm_dir_path,
			VBLK_EMU_CONFIG_ENV,
			VBLK_SHM_DIR_PATH_LEN - 1);
		return EXIT_FAILURE;
	}

	/* Role: "src" (default) or "dst" (passed as first positional arg) */
	enum vblk_ho_role role = VBLK_HO_ROLE_SRC;
	if (argc >= 2 && strcmp(argv[1], "dst") == 0)
		role = VBLK_HO_ROLE_DST;

	const char *role_str = (role == VBLK_HO_ROLE_DST) ? "DST" : "SRC";

	/* Logging */
	struct doca_log_backend *sdk_log = NULL;

	(void)doca_log_backend_create_standard();
	if (doca_log_backend_create_with_file_sdk(stderr, &sdk_log) == DOCA_SUCCESS)
		(void)doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

	/* SIGTERM: graceful shutdown (sent by TLP parent during normal teardown).
	 * SIGINT:  default action — immediate termination, no cleanup (ungraceful crash recovery flow). */
	signal(SIGTERM, signal_handler);

	/* Device init */
	struct doca_dev *dev;
	struct vblk_pci_virtio_dev *pci_dev = NULL;
	struct vblk_ipc_ep ipc;
	struct doca_devemu_pci_ep *ep;
	struct vblk_app_cfg app_cfg = {0};
	const char *bind_path;
	const char *peer_path;
	doca_error_t err;

	_Static_assert(sizeof(app_cfg.device_name) == sizeof(config.device_name), "size mismatch");
	memcpy(app_cfg.device_name, config.device_name, sizeof(app_cfg.device_name));
	app_cfg.indirect_enabled = config.indirect_enabled;
	app_cfg.datapath_on_dpa = config.datapath_on_dpa;
	err = vblk_init(&app_cfg, &dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("EMU %s: vblk_init failed: %s", role_str, doca_error_get_descr(err));
		return EXIT_FAILURE;
	}

	err = vblk_pci_type_init(dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("EMU %s: vblk_pci_type_init failed: %s", role_str, doca_error_get_descr(err));
		goto out_vblk;
	}

	pci_dev = vblk_pci_virtio_dev_open(config.num_queues);
	if (!pci_dev) {
		DOCA_LOG_ERR("EMU %s: failed to discover existing PCI endpoint", role_str);
		goto out_pci;
	}

	bind_path = (role == VBLK_HO_ROLE_DST) ? VBLK_IPC_EMU_DST_PATH : VBLK_IPC_EMU_SRC_PATH;
	peer_path = (role == VBLK_HO_ROLE_DST) ? VBLK_IPC_EMU_SRC_PATH : VBLK_IPC_TLP_PATH;

	err = vblk_ipc_ep_open(&ipc, bind_path, peer_path);
	if (err != DOCA_SUCCESS)
		goto out_ep;

	ep = doca_devemu_pci_tlp_dev_as_ep(pci_dev->pci_tlp_dev);
	DOCA_LOG_INFO("EMU %s: starting (pid=%d)", role_str, getpid());

	err = vblk_pci_dev_emu_run(&config, ep, &ipc, role);

	vblk_ipc_ep_close(&ipc);
out_ep:
	vblk_pci_virtio_dev_destroy(pci_dev);
out_pci:
	vblk_pci_type_reset();
out_vblk:
	vblk_reset();
	doca_dev_close(dev);
	return (err == DOCA_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}
