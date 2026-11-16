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
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <doca_log.h>
#include <doca_telemetry_exporter.h>

#define SOURCE_ID "doca_hft_metrics_src"
#define SOURCE_TAG "doca_hft_metrics"
#define NUM_METRICS 10
#define DEFAULT_LFT_US 1000000ULL
#define DEFAULT_IPC_DIR "/opt/mellanox/doca/services/telemetry/ipc_sockets"
#define DEFAULT_LFT_DATA "/tmp/doca_lft_data"
#define DEFAULT_PROM_PORT "9876"

DOCA_LOG_REGISTER(TELEMETRY_EXPORTER::METRICS_HFT_LFT);

static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig)
{
	(void)sig;
	g_running = 0;
}

static uint64_t parse_u64_env(const char *name, uint64_t default_value)
{
	const char *val;
	char *end = NULL;
	unsigned long long parsed;

	val = getenv(name);
	if (val == NULL || val[0] == '\0')
		return default_value;

	errno = 0;
	parsed = strtoull(val, &end, 10);
	if (errno != 0 || end == val || *end != '\0' || parsed == 0)
		return default_value;

	return (uint64_t)parsed;
}

static const char *env_or_default(const char *name, const char *default_value)
{
	const char *val = getenv(name);

	if (val == NULL || val[0] == '\0')
		return default_value;
	return val;
}

static doca_error_t ensure_dir(const char *path)
{
	struct stat st;

	if (mkdir(path, 0755) == 0)
		return DOCA_SUCCESS;
	if (errno != EEXIST) {
		DOCA_LOG_ERR("mkdir(%s) failed: %s", path, strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	/* Something is already there: only a directory will do as a data root. */
	if (stat(path, &st) != 0) {
		DOCA_LOG_ERR("stat(%s) failed: %s", path, strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	if (!S_ISDIR(st.st_mode)) {
		DOCA_LOG_ERR("%s exists and is not a directory", path);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

static doca_error_t emit_metrics(struct doca_telemetry_exporter_source *doca_source,
				 doca_telemetry_exporter_label_set_id_t label_set_id,
				 unsigned long tick)
{
	doca_error_t result;
	uint64_t timestamp = 0;
	int i;

	result = doca_telemetry_exporter_get_timestamp(&timestamp);
	if (result != DOCA_SUCCESS)
		return result;

	for (i = 1; i <= NUM_METRICS; i++) {
		char name[16];

		snprintf(name, sizeof(name), "counter%d", i);
		result = doca_telemetry_exporter_metrics_add_counter(doca_source,
								     timestamp,
								     name,
								     (uint64_t)(tick + (unsigned long)i),
								     label_set_id,
								     NULL);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return doca_telemetry_exporter_metrics_flush(doca_source);
}

/*
 * Dual-rate HFT/LFT metrics sample (mirrors CollectX clx_api_ipc_hft_lft_metrics).
 *
 * Loop: add counters → metrics_flush → doca_telemetry_exporter_sleep(lft_us).
 * Each flush force-writes one metrics page (NUM_METRICS counters fit a single
 * page — no multi-sample buffering), so the in-process Prometheus exporter
 * advances sample timestamps at the active sleep rate (LFT or HFT).
 * HFT is enabled only via DTS / CollectX management IPC command targeting SOURCE_TAG.
 *
 * Env knobs:
 *   LFT_US              - low-frequency period for sleep (default 1000000)
 *   IPC_SOCKETS_DIR     - DTS IPC sockets directory
 *   LFT_DATA_ROOT       - context file-writer root for LFT binaries
 *   APP_PROM_PORT       - in-app Prometheus listen port (default 9876)
 *   PROMETHEUS_ENDPOINT - full endpoint override (e.g. http://0.0.0.0:9876)
 *
 * Example HFT enable:
 *   POST /management/cmd {"ipc_command":{"target_tag":"doca_hft_metrics",
 *     "command":{"hft":{"enable":true,"mode":"session","duration_ms":30000,
 *                       "period_us":10000,"lft_period_us":1000000,
 *                       "data_root":"/tmp/doca_hft_data","exporters":["file"]}}}}
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t telemetry_export_metrics_hft_lft(void)
{
	doca_error_t result;
	struct doca_telemetry_exporter_schema *doca_schema = NULL;
	struct doca_telemetry_exporter_source *doca_source = NULL;
	doca_telemetry_exporter_label_set_id_t label_set_id = 0;
	uint64_t lft_us;
	const char *ipc_dir;
	const char *lft_data_root;
	const char *prom_endpoint_env;
	const char *prom_port;
	char prom_endpoint[256]; /* "http://<host>:<port>[/path]" */
	int written;
	unsigned long tick = 0;

	lft_us = parse_u64_env("LFT_US", DEFAULT_LFT_US);
	ipc_dir = env_or_default("IPC_SOCKETS_DIR", DEFAULT_IPC_DIR);
	lft_data_root = env_or_default("LFT_DATA_ROOT", DEFAULT_LFT_DATA);

	/* Prometheus endpoint: PROMETHEUS_ENDPOINT wins, else the in-app listener on APP_PROM_PORT. */
	prom_endpoint_env = getenv("PROMETHEUS_ENDPOINT");
	if (prom_endpoint_env == NULL || prom_endpoint_env[0] == '\0') {
		prom_port = env_or_default("APP_PROM_PORT", DEFAULT_PROM_PORT);
		written = snprintf(prom_endpoint, sizeof(prom_endpoint), "http://0.0.0.0:%s", prom_port);
	} else {
		written = snprintf(prom_endpoint, sizeof(prom_endpoint), "%s", prom_endpoint_env);
	}
	if (written < 0 || (size_t)written >= sizeof(prom_endpoint)) {
		DOCA_LOG_ERR("Prometheus endpoint is longer than %zu characters", sizeof(prom_endpoint) - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (signal(SIGINT, on_sigint) == SIG_ERR || signal(SIGTERM, on_sigint) == SIG_ERR) {
		DOCA_LOG_ERR("Cannot install the signal handlers: %s", strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	result = ensure_dir(lft_data_root);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("Starting dual-rate metrics sample: tag='%s' lft_us=%" PRIu64
		      " ipc_dir='%s' lft_data='%s' prometheus='%s/metrics' (%d counters/flush, one page)",
		      SOURCE_TAG,
		      lft_us,
		      ipc_dir,
		      lft_data_root,
		      prom_endpoint,
		      NUM_METRICS);

	result = doca_telemetry_exporter_schema_init("metrics_hft_lft_schema", &doca_schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot init doca schema");
		return result;
	}

	/* LFT on-disk stream (context file writer); HFT file path comes from IPC hft command. */
	doca_telemetry_exporter_schema_set_file_write_enabled(doca_schema);
	doca_telemetry_exporter_schema_set_buf_data_root(doca_schema, lft_data_root);
	/* Prometheus through the public API: set its endpoint option, then enable it (which checks the endpoint). */
	result = doca_telemetry_exporter_schema_set_exporter_option(doca_schema, "prometheus-endpoint", prom_endpoint);
	if (result == DOCA_SUCCESS)
		result = doca_telemetry_exporter_schema_set_exporter_enabled(doca_schema, "prometheus");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot configure the Prometheus exporter: %s", doca_error_get_descr(result));
		goto err_schema;
	}

	doca_telemetry_exporter_schema_set_ipc_enabled(doca_schema);
	doca_telemetry_exporter_schema_set_ipc_sockets_dir(doca_schema, ipc_dir);

	result = doca_telemetry_exporter_schema_start(doca_schema);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start doca_schema");
		goto err_schema;
	}

	result = doca_telemetry_exporter_source_create(doca_schema, &doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create doca_source");
		goto err_schema;
	}

	doca_telemetry_exporter_source_set_id(doca_source, SOURCE_ID);
	doca_telemetry_exporter_source_set_tag(doca_source, SOURCE_TAG);

	result = doca_telemetry_exporter_source_start(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot start doca_source");
		goto err_source;
	}

	result = doca_telemetry_exporter_metrics_create_context(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot create metrics context");
		goto err_source;
	}

	/* Empty label set handle required by add_counter; no dynamic dimensions. */
	result = doca_telemetry_exporter_metrics_add_label_names(doca_source, NULL, 0, &label_set_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Cannot add label names");
		goto err_metrics;
	}

	DOCA_LOG_INFO("Waiting for hft command; sampling at LFT via doca_telemetry_exporter_sleep");
	DOCA_LOG_INFO("Prometheus timestamps track flush rate (one page per sample)");
	DOCA_LOG_INFO("Disable with {\"hft\":{\"enable\":false}} on target_tag='%s'", SOURCE_TAG);

	while (g_running) {
		result = emit_metrics(doca_source, label_set_id, tick++);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to emit metrics");
			goto err_metrics;
		}

		result = doca_telemetry_exporter_sleep(doca_source, lft_us);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_telemetry_exporter_sleep failed");
			goto err_metrics;
		}
	}

	result = doca_telemetry_exporter_metrics_destroy_context(doca_source);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy metrics context");
		goto err_source;
	}

	doca_telemetry_exporter_source_destroy(doca_source);
	doca_telemetry_exporter_schema_destroy(doca_schema);

	DOCA_LOG_INFO("Dual-rate metrics sample completed successfully");
	return DOCA_SUCCESS;

err_metrics:
	doca_telemetry_exporter_metrics_destroy_context(doca_source);
err_source:
	doca_telemetry_exporter_source_destroy(doca_source);
err_schema:
	doca_telemetry_exporter_schema_destroy(doca_schema);
	return result;
}
