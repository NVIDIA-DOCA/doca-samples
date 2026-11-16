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

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

#include "memos_initiator_ref_app_core.h"
#include "memos_initiator_ref_app_worker.h" /* MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER */

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP);

static volatile sig_atomic_t g_force_quit = 0;

static void memos_initiator_ref_app_signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM)
		g_force_quit = 1;
}

static doca_error_t pci_addr_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const char *addr = param;
	const size_t len = strnlen(addr, DOCA_DEVINFO_PCI_ADDR_SIZE);

	if (len == 0 || len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("--pci-addr value must be 1..%d chars", DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(cfg->pci_address, addr, len + 1);
	return DOCA_SUCCESS;
}

static doca_error_t vuid_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const char *vuid = param;
	const size_t len = strnlen(vuid, DOCA_DEVINFO_REP_VUID_SIZE);

	if (len == 0 || len >= DOCA_DEVINFO_REP_VUID_SIZE) {
		DOCA_LOG_ERR("--vuid value must be 1..%d chars", DOCA_DEVINFO_REP_VUID_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(cfg->vuid, vuid, len + 1);
	return DOCA_SUCCESS;
}

/* Copy a mandatory target-identifying string into cfg, rejecting empty or over-long values. The
 * four --traddr / --trsvcid / --subnqn / --hostnqn callbacks below differ only in destination. */
static doca_error_t target_string_copy(const char *long_name, const char *value, char *dst, size_t dst_size)
{
	const size_t len = strnlen(value, dst_size);

	if (len == 0 || len >= dst_size) {
		DOCA_LOG_ERR("--%s value must be 1..%zu chars", long_name, dst_size - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(dst, value, len + 1);
	return DOCA_SUCCESS;
}

static doca_error_t rdma_dev_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	return target_string_copy("rdma-dev", param, cfg->rdma_dev, sizeof(cfg->rdma_dev));
}

static doca_error_t traddr_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	return target_string_copy("traddr", param, cfg->traddr, sizeof(cfg->traddr));
}

static doca_error_t trsvcid_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	return target_string_copy("trsvcid", param, cfg->trsvcid, sizeof(cfg->trsvcid));
}

static doca_error_t subnqn_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	return target_string_copy("subnqn", param, cfg->subnqn, sizeof(cfg->subnqn));
}

static doca_error_t hostnqn_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	return target_string_copy("hostnqn", param, cfg->hostnqn, sizeof(cfg->hostnqn));
}

static doca_error_t list_emulated_functions_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	(void)param;
	cfg->list_emulated_functions = true;
	return DOCA_SUCCESS;
}

static doca_error_t hotplug_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	(void)param;
	cfg->hotplug = true;
	return DOCA_SUCCESS;
}

static doca_error_t hotunplug_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const char *vuid = param;
	const size_t len = strnlen(vuid, DOCA_DEVINFO_REP_VUID_SIZE);

	if (len == 0 || len >= DOCA_DEVINFO_REP_VUID_SIZE) {
		DOCA_LOG_ERR("--hotunplug value must be 1..%d chars", DOCA_DEVINFO_REP_VUID_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(cfg->hotunplug_vuid, vuid, len + 1);
	cfg->hotunplug = true;
	return DOCA_SUCCESS;
}

static doca_error_t pci_type_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const char *kind = param;

	if (strcmp(kind, "generic_nvme") == 0) {
		cfg->pci_type_kind = MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_GENERIC_NVME;
	} else if (strcmp(kind, "generic") == 0) {
		cfg->pci_type_kind = MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_GENERIC;
	} else if (strcmp(kind, "nvme") == 0) {
		cfg->pci_type_kind = MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_NATIVE;
	} else {
		DOCA_LOG_ERR("--pci-type value must be 'generic_nvme', 'generic' or 'nvme', got '%s'", kind);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

static doca_error_t num_io_queues_per_core_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const int value = *(const int *)param;

	if (value < 1) {
		DOCA_LOG_ERR("--num-io-queues-per-core must be >= 1");
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->num_io_queues_per_core = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t io_defer_count_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const int value = *(const int *)param;

	if (value < 0 || value > MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER) {
		DOCA_LOG_ERR("--io-defer-count must be 0..%d", MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->io_defer_count = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t io_defer_keep_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const int value = *(const int *)param;

	if (value < 0 || value >= MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER) {
		DOCA_LOG_ERR("--io-defer-keep must be 0..%d", MEMOS_INITIATOR_REF_APP_MAX_IO_DEFER - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->io_defer_keep = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t io_defer_max_us_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const int value = *(const int *)param;

	if (value < 1 || (uint32_t)value > MEMOS_INITIATOR_REF_APP_IO_DEFER_MAX_US_LIMIT) {
		DOCA_LOG_ERR("--io-defer-max-us must be 1..%u", MEMOS_INITIATOR_REF_APP_IO_DEFER_MAX_US_LIMIT);
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->io_defer_max_us = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t io_defer_reverse_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;

	(void)param;
	cfg->io_defer_reverse = true;
	return DOCA_SUCCESS;
}

static doca_error_t core_mask_callback(void *param, void *config)
{
	struct memos_initiator_ref_app_cfg *cfg = config;
	const char *str = param;
	char *end = NULL;
	unsigned long long mask;

	/* base 0: accept hex (0x1), octal and decimal masks. A mask never carries a sign, and strtoull()
	 * would fold one into a full set of cores rather than refusing it, so '-' is rejected outright;
	 * ERANGE catches a value too large for the mask instead of letting it saturate. */
	errno = 0;
	mask = strtoull(str, &end, 0);
	if (end == str || *end != '\0' || mask == 0 || errno == ERANGE || strchr(str, '-') != NULL) {
		DOCA_LOG_ERR("--core-mask must be a non-zero CPU bitmask (e.g. 0x1, 0x3, 6)");
		return DOCA_ERROR_INVALID_VALUE;
	}
	cfg->core_mask = (uint64_t)mask;
	return DOCA_SUCCESS;
}

static doca_error_t memos_initiator_ref_app_register_string_param(const char *short_name,
								  const char *long_name,
								  const char *description,
								  doca_argp_param_cb_t cb,
								  bool mandatory)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create --%s param: %s", long_name, doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, short_name);
	doca_argp_param_set_long_name(param, long_name);
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, cb);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	if (mandatory)
		doca_argp_param_set_mandatory(param);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to register --%s param: %s", long_name, doca_error_get_descr(result));
	return result;
}

static doca_error_t memos_initiator_ref_app_register_int_param(const char *short_name,
							       const char *long_name,
							       const char *description,
							       doca_argp_param_cb_t cb)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create --%s param: %s", long_name, doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, short_name);
	doca_argp_param_set_long_name(param, long_name);
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, cb);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to register --%s param: %s", long_name, doca_error_get_descr(result));
	return result;
}

static doca_error_t memos_initiator_ref_app_register_bool_param(const char *short_name,
								const char *long_name,
								const char *description,
								doca_argp_param_cb_t cb)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create --%s param: %s", long_name, doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, short_name);
	doca_argp_param_set_long_name(param, long_name);
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, cb);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to register --%s param: %s", long_name, doca_error_get_descr(result));
	return result;
}

static doca_error_t register_params(void)
{
	doca_error_t result;

	result = memos_initiator_ref_app_register_string_param(
		"p",
		"pci-addr",
		"DOCA device PCI address (the NVMe emulation manager). Format: XXXX:XX:XX.X",
		pci_addr_callback,
		/* mandatory */ true);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_string_param(
		"u",
		"vuid",
		"VUID of the emulated NVMe device representor (required unless --list-emulated-functions)",
		vuid_callback,
		/* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	/* The four below identify the NVMe-oF target that backs the KV namespace. They are not
	 * mandatory at the argp level because --list-emulated-functions and --hotplug exit before any
	 * target is needed; memos_initiator_ref_app_kv_init() requires them when it actually connects. */
	result = memos_initiator_ref_app_register_string_param(
		"R",
		"rdma-dev",
		"ibdev name of the RDMA device with a route to the target (e.g. mlx5_2); not the --pci-addr device",
		rdma_dev_callback,
		/* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_string_param(
		"a",
		"traddr",
		"NVMe-oF target IPv4 address (required to serve the KV namespace)",
		traddr_callback,
		/* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_string_param("s",
							       "trsvcid",
							       "NVMe-oF target service id (port), e.g. 4420",
							       trsvcid_callback,
							       /* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_string_param("n",
							       "subnqn",
							       "NQN of the target subsystem exporting the KV namespace",
							       subnqn_callback,
							       /* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_string_param("N",
							       "hostnqn",
							       "NQN this initiator presents to the target",
							       hostnqn_callback,
							       /* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_bool_param(
		"L",
		"list-emulated-functions",
		"List every emulated function (NVMe / generic / virtio_*) managed by --pci-addr and exit",
		list_emulated_functions_callback);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_bool_param(
		"H",
		"hotplug",
		"Create a new emulated function (left POWER_OFF) under the app pci_type, log its VUID and exit. Power it on with a regular run (--vuid)",
		hotplug_callback);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_string_param(
		"U",
		"hotunplug",
		"Power off (if needed) and destroy the emulated function with the given VUID",
		hotunplug_callback,
		/* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_string_param(
		"T",
		"pci-type",
		"PCI type for hotplugged functions: 'generic_nvme' (default) and 'generic' are backed by generic emulation, taking the NVMe BAR layout from firmware (pci_type DOCA_DEVEMU_NVME) and from the app (pci_type DOCA_DEVEMU_NVME_GEN) respectively, the latter for firmware that does not offer the canned layout; 'nvme' is native NVMe emulation, where firmware owns BAR0 and the library shares one type per device. A function is only drivable under the pci_type it was created with, and nothing reports which that was - keep track of it when you hotplug",
		pci_type_callback,
		/* mandatory */ false);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_int_param("q",
							    "num-io-queues-per-core",
							    "Number of IO queues handled per worker core (default 1)",
							    num_io_queues_per_core_callback);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_int_param(
		"d",
		"io-defer-count",
		"[debug] Hold this many IO completions and release them as a batch (0 = complete inline, default 0). Exercises delayed completion",
		io_defer_count_callback);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_int_param(
		"k",
		"io-defer-keep",
		"[debug] Keep this many oldest requests held when an --io-defer-count batch is released (default 0). Holding one while newer commands complete lets the host's SQ tail wrap onto its slot",
		io_defer_keep_callback);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_int_param(
		"t",
		"io-defer-max-us",
		"[debug] Microseconds the --io-defer-keep requests may be withheld before being released anyway (default 1000). Raise it to let the SQ tail wrap; other completions are released as soon as the worker goes idle, so this does not throttle normal IO",
		io_defer_max_us_callback);
	if (result != DOCA_SUCCESS)
		return result;
	result = memos_initiator_ref_app_register_bool_param(
		"r",
		"io-defer-reverse",
		"[debug] Release each --io-defer-count batch newest-first, so a later command completes before earlier ones. Exercises out-of-order completion",
		io_defer_reverse_callback);
	if (result != DOCA_SUCCESS)
		return result;
	return memos_initiator_ref_app_register_string_param(
		"m",
		"core-mask",
		"CPU bitmask of cores running IO datapath worker threads, e.g. 0x1, 0x3 (default 0x1)",
		core_mask_callback,
		/* mandatory */ false);
}

int main(int argc, char **argv)
{
	struct memos_initiator_ref_app_resources app = {0};
	struct doca_log_backend *sdk_log;
	doca_error_t result;
	int exit_status = EXIT_FAILURE;
	/* Declared here rather than where they are first computed: the argument checks in between jump
	 * to destroy_argp, and those jumps would otherwise cross their initialization. */
	bool run_action;
	int action_count;

	app.force_quit = &g_force_quit;

	/* Defaults for the optional params (argp callbacks only fire when supplied). */
	app.cfg.num_io_queues_per_core = 1;
	app.cfg.core_mask = 0x1;
	app.cfg.io_defer_max_us = MEMOS_INITIATOR_REF_APP_IO_DEFER_MAX_US;

	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto exit_app;
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto exit_app;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto exit_app;

	DOCA_LOG_INFO("Starting the DOCA MEMOS initiator reference application");

	result = doca_argp_init("doca_memos_initiator_ref_app", &app.cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP: %s", doca_error_get_descr(result));
		goto exit_app;
	}
	result = register_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register params: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse arguments: %s", doca_error_get_descr(result));
		goto destroy_argp;
	}

	run_action = app.cfg.vuid[0] != '\0';
	action_count =
		(int)app.cfg.list_emulated_functions + (int)app.cfg.hotplug + (int)app.cfg.hotunplug + (int)run_action;

	if (action_count == 0) {
		DOCA_LOG_ERR("One of --vuid, --list-emulated-functions, --hotplug, --hotunplug VUID must be specified");
		goto destroy_argp;
	}
	if (action_count > 1) {
		DOCA_LOG_ERR("--vuid, --list-emulated-functions, --hotplug, --hotunplug are mutually exclusive");
		goto destroy_argp;
	}

	/* Checked here rather than in the callbacks: argp invokes them in command-line order, so
	 * neither value is guaranteed to be parsed when the other is validated. */
	if (app.cfg.io_defer_keep >= app.cfg.io_defer_count && app.cfg.io_defer_keep > 0) {
		DOCA_LOG_ERR(
			"--io-defer-keep (%u) must be less than --io-defer-count (%u), else no batch is ever released",
			app.cfg.io_defer_keep,
			app.cfg.io_defer_count);
		goto destroy_argp;
	}

	signal(SIGINT, memos_initiator_ref_app_signal_handler);
	signal(SIGTERM, memos_initiator_ref_app_signal_handler);

	if (app.cfg.list_emulated_functions) {
		result = memos_initiator_ref_app_list_emulated_functions(&app);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("memos_initiator_ref_app_list_emulated_functions failed: %s",
				     doca_error_get_descr(result));
		else
			exit_status = EXIT_SUCCESS;
		goto destroy_argp;
	}

	if (app.cfg.hotplug) {
		result = memos_initiator_ref_app_hotplug(&app);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("memos_initiator_ref_app_hotplug failed: %s", doca_error_get_descr(result));
		else
			exit_status = EXIT_SUCCESS;
		goto destroy_argp;
	}

	if (app.cfg.hotunplug) {
		result = memos_initiator_ref_app_hotunplug(&app);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("memos_initiator_ref_app_hotunplug failed: %s", doca_error_get_descr(result));
		else
			exit_status = EXIT_SUCCESS;
		goto destroy_argp;
	}

	result = memos_initiator_ref_app_run(&app);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("memos_initiator_ref_app_run failed: %s", doca_error_get_descr(result));
	else
		exit_status = EXIT_SUCCESS;

destroy_argp:
	doca_argp_destroy();
exit_app:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("doca_memos_initiator_ref_app exited cleanly");
	else
		DOCA_LOG_INFO("doca_memos_initiator_ref_app exited with errors");
	return exit_status;
}
