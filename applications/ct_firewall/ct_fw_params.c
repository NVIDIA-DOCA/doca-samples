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

/* CT Firewall command-line options and their parse helpers. */

#include <string.h>

#include <arpa/inet.h>

#include <doca_argp.h>
#include <doca_log.h>

#include "ct_fw.h"

DOCA_LOG_REGISTER(CT_FIREWALL::PARAMS);

static doca_error_t sync_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;

	if (*(const bool *)param)
		cfg->sync_mode = CT_FW_SYNC;
	return DOCA_SUCCESS;
}

static doca_error_t no_window_validation_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;

	cfg->enable_window_validation = !*(const bool *)param;
	return DOCA_SUCCESS;
}

static doca_error_t liberal_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;

	cfg->liberal_mode = *(const bool *)param;
	return DOCA_SUCCESS;
}

static doca_error_t max_ack_window_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;
	const int value = *(const int *)param;
	doca_error_t result = ct_fw_config_check_int_range("max-ack-window", value, 0, CT_FW_MAX_ACK_WINDOW);

	if (result != DOCA_SUCCESS)
		return result;
	cfg->max_ack_window = (uint8_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t offload_after_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;
	const int value = *(const int *)param;
	doca_error_t result = ct_fw_config_check_int_range("offload-after", value, 0, CT_FW_MAX_OFFLOAD_AFTER);

	if (result != DOCA_SUCCESS)
		return result;
	cfg->offload_after = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t dnat_callback(void *param, void *config)
{
	return ct_fw_config_parse_nat_addr("dnat", param, true, config);
}

static doca_error_t snat_callback(void *param, void *config)
{
	return ct_fw_config_parse_nat_addr("snat", param, false, config);
}

static doca_error_t ct_queues_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;
	const int value = *(const int *)param;
	doca_error_t result = ct_fw_config_check_int_range("ct-queues", value, 1, CT_FW_MAX_WORKERS);

	if (result != DOCA_SUCCESS)
		return result;
	cfg->nb_ct_queues = (uint16_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t sessions_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;
	const int value = *(const int *)param;
	doca_error_t result = ct_fw_config_check_int_range("sessions", value, 1, CT_FW_MAX_SESSIONS);

	if (result != DOCA_SUCCESS)
		return result;
	cfg->nb_sessions = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t pool_size_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;
	const int value = *(const int *)param;
	doca_error_t result = ct_fw_config_check_int_range("pool-size", value, 1, CT_FW_MAX_POOL);

	if (result != DOCA_SUCCESS)
		return result;
	cfg->pool_size = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t aging_timeout_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;
	const int value = *(const int *)param;
	doca_error_t result = ct_fw_config_check_int_range("aging-timeout", value, 1, CT_FW_MAX_AGING_TIMEOUT_S);

	if (result != DOCA_SUCCESS)
		return result;
	cfg->aging_timeout_s = (uint32_t)value;
	return DOCA_SUCCESS;
}

static doca_error_t stats_interval_callback(void *param, void *config)
{
	struct ct_fw_config *cfg = config;
	const int value = *(const int *)param;
	doca_error_t result = ct_fw_config_check_int_range("stats-interval", value, 0, CT_FW_MAX_STATS_INTERVAL);

	if (result != DOCA_SUCCESS)
		return result;
	cfg->stats_interval = (uint32_t)value;
	return DOCA_SUCCESS;
}

struct ct_fw_param {
	const char *long_name;
	const char *arguments;	 /**< NULL for a flag */
	const char *description; /**< what it does; range; default */
	enum doca_argp_type type;
	doca_argp_param_cb_t callback;
};

/* Registration order is the help order. */
static const struct ct_fw_param ct_fw_params[] = {
	{"sync",
	 NULL,
	 "Install connections synchronously (the worker waits for each hardware install and removal); default asynchronous",
	 DOCA_ARGP_TYPE_BOOLEAN,
	 sync_callback},
	{"no-window-validation",
	 NULL,
	 "Disable TCP window validation; connections are tracked and forwarded without sequence checks",
	 DOCA_ARGP_TYPE_BOOLEAN,
	 no_window_validation_callback},
	{"liberal",
	 NULL,
	 "Disable strict window validation on every connection (out-of-window segments are not dropped)",
	 DOCA_ARGP_TYPE_BOOLEAN,
	 liberal_callback},
	{"max-ack-window",
	 "<n>",
	 "Acknowledgement-window bound exponent, 0 selects the default 64 KiB; 0..7; default 0",
	 DOCA_ARGP_TYPE_INT,
	 max_ack_window_callback},
	{"offload-after",
	 "<bytes>",
	 "Keep a TCP connection in software until this many payload bytes passed, 0 installs at the SYN-ACK; "
	 "0..2147483647; default 0",
	 DOCA_ARGP_TYPE_INT,
	 offload_after_callback},
	{"dnat",
	 "<ip>",
	 "Rewrite the destination of connections opened from the first uplink to <ip> (same IP version only; "
	 "needs --ct-queues 1); IPv4 or IPv6 address; default none",
	 DOCA_ARGP_TYPE_STRING,
	 dnat_callback},
	{"snat",
	 "<ip>",
	 "Rewrite the source of connections opened from the first uplink to <ip> (same IP version only; "
	 "needs --ct-queues 1); IPv4 or IPv6 address; default none",
	 DOCA_ARGP_TYPE_STRING,
	 snat_callback},
	{"ct-queues",
	 "<n>",
	 "Connection-tracking queues, one worker thread each; 1..16; default 1",
	 DOCA_ARGP_TYPE_INT,
	 ct_queues_callback},
	{"sessions",
	 "<n>",
	 "Connection-tracking sessions per address family, aggregate across queues; 1..3800000; default 8192",
	 DOCA_ARGP_TYPE_INT,
	 sessions_callback},
	{"pool-size",
	 "<n>",
	 "Maximum TCP connections offloaded with window validation at once; 1..8388608; default 8192",
	 DOCA_ARGP_TYPE_INT,
	 pool_size_callback},
	{"aging-timeout",
	 "<n>",
	 "Seconds a connection may stay idle before it is released; 1..65535; default 30",
	 DOCA_ARGP_TYPE_INT,
	 aging_timeout_callback},
	{"stats-interval",
	 "<n>",
	 "Seconds between periodic statistics reports, 0 disables; 0..3600; default 5",
	 DOCA_ARGP_TYPE_INT,
	 stats_interval_callback},
};

static doca_error_t register_single_param(const struct ct_fw_param *def)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param %s: %s", def->long_name, doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, def->long_name);
	if (def->arguments != NULL)
		doca_argp_param_set_arguments(param, def->arguments);
	doca_argp_param_set_description(param, def->description);
	doca_argp_param_set_callback(param, def->callback);
	doca_argp_param_set_type(param, def->type);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to register ARGP param %s: %s", def->long_name, doca_error_get_descr(result));
	return result;
}

static struct flow_dev_ctx *ct_fw_dev_ctx_from_user_ctx(void *user_ctx)
{
	return &((struct ct_fw_config *)user_ctx)->switch_ctx.devs_ctx;
}

doca_error_t ct_fw_config_check_int_range(const char *name, int value, uint32_t min, uint32_t max)
{
	if (value < 0 || (uint32_t)value < min || (uint32_t)value > max) {
		DOCA_LOG_ERR("--%s must be in %u..%u (got %d)", name, min, max, value);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

doca_error_t ct_fw_config_parse_nat_addr(const char *name, const char *literal, bool dst, struct ct_fw_config *cfg)
{
	uint8_t addr[CT_FW_IPV6_ADDR_BYTES] = {0};
	enum ct_fw_family family, other;
	struct ct_fw_nat *nat;

	if (inet_pton(AF_INET, literal, addr) == 1) {
		family = CT_FW_FAMILY_IPV4;
	} else if (inet_pton(AF_INET6, literal, addr) == 1) {
		family = CT_FW_FAMILY_IPV6;
	} else {
		DOCA_LOG_ERR("--%s: '%s' is not an IPv4 or IPv6 address", name, literal);
		return DOCA_ERROR_INVALID_VALUE;
	}
	/* All-zero means "not translated". */
	if (!ct_fw_nat_addr_set(addr, family)) {
		DOCA_LOG_ERR("--%s: '%s' is the unspecified address; a translation target must be a real address",
			     name,
			     literal);
		return DOCA_ERROR_INVALID_VALUE;
	}
	/* Both flags must name one family. */
	other = (family == CT_FW_FAMILY_IPV4) ? CT_FW_FAMILY_IPV6 : CT_FW_FAMILY_IPV4;
	if (cfg->nat[other].enabled) {
		DOCA_LOG_ERR("--%s: '%s' is IPv%c but the other translation address is IPv%c; --dnat and --snat must "
			     "name the same address family",
			     name,
			     literal,
			     family == CT_FW_FAMILY_IPV4 ? '4' : '6',
			     other == CT_FW_FAMILY_IPV4 ? '4' : '6');
		return DOCA_ERROR_INVALID_VALUE;
	}
	nat = &cfg->nat[family];
	memcpy(dst ? nat->new_dst : nat->new_src, addr, sizeof(addr));
	nat->enabled = true;
	return DOCA_SUCCESS;
}

doca_error_t ct_fw_config_validate(const struct ct_fw_config *cfg)
{
	bool nat = cfg->nat[CT_FW_FAMILY_IPV4].enabled || cfg->nat[CT_FW_FAMILY_IPV6].enabled;

	/* A reply could reach another worker's table. */
	if (nat && cfg->nb_ct_queues > 1) {
		DOCA_LOG_ERR("Address translation (--dnat/--snat) requires a single CT queue (--ct-queues 1)");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (!cfg->enable_window_validation && (cfg->liberal_mode || cfg->max_ack_window != 0)) {
		DOCA_LOG_ERR(
			"--liberal and --max-ack-window require TCP window validation; remove --no-window-validation");
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

doca_error_t ct_fw_register_params(void)
{
	doca_error_t result;
	size_t i;

	result = register_flow_device_params(ct_fw_dev_ctx_from_user_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register DOCA Flow switch device params: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < sizeof(ct_fw_params) / sizeof(ct_fw_params[0]); i++) {
		result = register_single_param(&ct_fw_params[i]);
		if (result != DOCA_SUCCESS)
			return result;
	}
	return DOCA_SUCCESS;
}
