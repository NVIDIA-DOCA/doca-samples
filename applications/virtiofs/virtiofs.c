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

#include <stdbool.h>
#include <signal.h>

#include <unistd.h>
#include <doca_log.h>
#include <doca_argp.h>
#include <utils.h>

#include <virtiofs_core.h>

DOCA_LOG_REGISTER(VIRTIOFS)

volatile bool force_quit;
bool skip_rw;

/*
 * ARGP Callback - Handle core mask parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t core_mask_callback(void *param, void *config)
{
	struct virtiofs_cfg *conf = (struct virtiofs_cfg *)config;
	char *core_mask = (char *)param;

	if (strnlen(core_mask, VIRTIOFS_CORE_MASK_SIZE) == VIRTIOFS_CORE_MASK_SIZE) {
		DOCA_LOG_ERR("core mask lenght is too long - MAX=%d", VIRTIOFS_CORE_MASK_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(conf->core_mask, core_mask, VIRTIOFS_CORE_MASK_SIZE);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle NFS server parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nfs_server_callback(void *param, void *config)
{
	struct virtiofs_cfg *conf = (struct virtiofs_cfg *)config;
	char *nfs_server = (char *)param;

	if (strnlen(nfs_server, VIRTIOFS_NFS_SERVER_SIZE) == VIRTIOFS_NFS_SERVER_SIZE) {
		DOCA_LOG_ERR("NFS server argument too long, must be <=%u long", VIRTIOFS_NFS_SERVER_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(conf->nfs_server, nfs_server, VIRTIOFS_NFS_SERVER_SIZE);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle NFS export parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nfs_export_callback(void *param, void *config)
{
	struct virtiofs_cfg *conf = (struct virtiofs_cfg *)config;
	char *nfs_export = (char *)param;

	if (strnlen(nfs_export, VIRTIOFS_NFS_EXPORT_SIZE) == VIRTIOFS_NFS_EXPORT_SIZE) {
		DOCA_LOG_ERR("NFS export argument too long, must be <=%u long", VIRTIOFS_NFS_EXPORT_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strlcpy(conf->nfs_export, nfs_export, VIRTIOFS_NFS_EXPORT_SIZE);
	return DOCA_SUCCESS;
}

/*
 * Registers all flags used by the application for DOCA argument parser, so that when parsing
 * it can be parsed accordingly
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_... otherwise
 */
static doca_error_t register_virtiofs_params(void)
{
	doca_error_t result;
	struct doca_argp_param *core_mask, *nfs_server, *nfs_export;

	/* Create and register core mask param */
	result = doca_argp_param_create(&core_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(core_mask, "m");
	doca_argp_param_set_long_name(core_mask, "core-mask");
	doca_argp_param_set_arguments(core_mask, "<core_mask>");
	doca_argp_param_set_description(core_mask, "Set core mask.");
	doca_argp_param_set_callback(core_mask, core_mask_callback);
	doca_argp_param_set_type(core_mask, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(core_mask);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register NFS server param */
	result = doca_argp_param_create(&nfs_server);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(nfs_server, "s");
	doca_argp_param_set_long_name(nfs_server, "nfs-server");
	doca_argp_param_set_arguments(nfs_server, "<nfs_server>");
	doca_argp_param_set_description(nfs_server, "Set NFS server.");
	doca_argp_param_set_callback(nfs_server, nfs_server_callback);
	doca_argp_param_set_type(nfs_server, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(nfs_server);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register NFS export param */
	result = doca_argp_param_create(&nfs_export);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(nfs_export, "e");
	doca_argp_param_set_long_name(nfs_export, "nfs-export");
	doca_argp_param_set_arguments(nfs_export, "<nfs_export>");
	doca_argp_param_set_description(nfs_export, "Set NFS export.");
	doca_argp_param_set_callback(nfs_export, nfs_export_callback);
	doca_argp_param_set_type(nfs_export, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(nfs_export);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize signal handling for main thread: build the wait set and block it
 * so signals are delivered via sigwait.
 *
 * @param sigset [out]: signal set to populate
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_UNKNOWN otherwise
 */
static doca_error_t virtiofs_signals_setup(sigset_t *sigset)
{
	int rc;

	if (sigemptyset(sigset) != 0)
		return DOCA_ERROR_UNKNOWN;
	if (sigaddset(sigset, SIGINT) != 0)
		return DOCA_ERROR_UNKNOWN;
	if (sigaddset(sigset, SIGTERM) != 0)
		return DOCA_ERROR_UNKNOWN;

	rc = pthread_sigmask(SIG_BLOCK, sigset, NULL);
	if (rc != 0) {
		DOCA_LOG_ERR("Failed to block signals for sigwait: %s", strerror(rc));
		return DOCA_ERROR_UNKNOWN;
	}

	return DOCA_SUCCESS;
}

static bool virtiofs_skip_rw(void)
{
	const char *env_var = NULL;
	const char *name = "DOCA_VIRTIOFS_USE_NULL_NFS_FSDEV";

	env_var = getenv(name);
	if (env_var == NULL) {
		DOCA_LOG_INFO("env_var:%s not found; read/write will be submitted to NFS fsdev", name);
		return false;
	}

	if (strcmp(env_var, "1") == 0) {
		DOCA_LOG_INFO("env_var:%s set by user; read/write will be submitted to NULL fsdev", name);
		return true;
	} else if (strcmp(env_var, "0") == 0) {
		DOCA_LOG_INFO("env_var:%s not set by user; read/write will be submitted to NFS fsdev", name);
		return false;
	} else {
		DOCA_LOG_INFO("invalid value:%s used to set env_var:%s", env_var, name);
		return false;
	}

	return false;
}

/*
 * VirtioFS application main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct virtiofs_cfg app_cfg = {.core_mask = VIRTIOFS_CORE_MASK_DEFAULT,
				       .nfs_server = VIRTIOFS_NFS_SERVER_DEFAULT,
				       .nfs_export = VIRTIOFS_NFS_EXPORT_DEFAULT};
	struct doca_log_backend *sdk_log;
	doca_error_t result;
	struct virtiofs_resources *ctx;
	sigset_t sigset;
	int sig, ret;
	int exit_status = EXIT_SUCCESS;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		return EXIT_FAILURE;

	/* Parse cmdline/json arguments */
	result = doca_argp_init(NULL, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	/* Register application parameters */
	result = register_virtiofs_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register application parameters: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto destroy_argp;
	}

	/* Start Arg Parser */
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse application input: %s", doca_error_get_descr(result));
		exit_status = EXIT_FAILURE;
		goto destroy_argp;
	}

	ctx = virtiofs_create((uint32_t)strtol(app_cfg.core_mask, NULL, 16));
	if (ctx == NULL) {
		DOCA_LOG_ERR("Failed to create global doca context");
		exit_status = EXIT_FAILURE;
		goto destroy_argp;
	}

	result = virtiofs_start(ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start doca context");
		exit_status = EXIT_FAILURE;
		goto cleanup_ctx;
	}

	skip_rw = virtiofs_skip_rw();

	result = virtiofs_device_create_static(ctx, app_cfg.nfs_server, app_cfg.nfs_export);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create static devices");
		exit_status = EXIT_FAILURE;
		goto cleanup_ctx;
	}

	result = virtiofs_signals_setup(&sigset);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to setup signal handling for sigwait");
		exit_status = EXIT_FAILURE;
		goto cleanup_ctx;
	}

	while (!force_quit) {
		ret = sigwait(&sigset, &sig);
		if (ret) {
			DOCA_LOG_ERR("Failed to sigwait: %s", strerror(ret));
			return result;
		}

		switch (sig) {
		case SIGINT:
		case SIGTERM:
			force_quit = true;
			break;
		default:
			DOCA_LOG_WARN("Polled unexpected signal %d", sig);
			break;
		}
	}

	virtiofs_stop(ctx);
	DOCA_LOG_INFO("Application stopped");

	virtiofs_destroy(ctx);
	DOCA_LOG_INFO("VirtioFS app finished successfully");
	exit(EXIT_SUCCESS);

cleanup_ctx:
	virtiofs_stop(ctx);
	virtiofs_destroy(ctx);

destroy_argp:
	doca_argp_destroy();

	return exit_status;
}
