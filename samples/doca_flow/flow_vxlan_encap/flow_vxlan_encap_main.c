/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdlib.h>

#include <doca_argp.h>
#include <doca_flow.h>
#include <doca_log.h>

#include <flow_common.h>

#include <dpdk_utils.h>

DOCA_LOG_REGISTER(FLOW_VXLAN_ENCAP::MAIN);

/* Sample's Logic */
doca_error_t flow_vxlan_encap(int nb_queues, enum doca_flow_tun_ext_vxlan_type vxlan_type);

/*
 * Config for vxlan_encap params
 */
struct vxlan_encap_config {
	struct flow_dev_ctx flow_dev_ctx;	      /* Basic flow device context*/
	enum doca_flow_tun_ext_vxlan_type vxlan_type; /* VXLAN type for the flow to create */
};

/*
 * Callback for arg vxlan-type
 *
 * @param [in]: parameter
 * @config [in]: app dpdk config
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vxlan_type_callback(void *param, void *config)
{
	struct vxlan_encap_config *app_config = (struct vxlan_encap_config *)config;
	const char *str = (const char *)param;

	if (strcmp(str, "gbp") == 0)
		app_config->vxlan_type = DOCA_FLOW_TUN_EXT_VXLAN_GBP;
	else if (strcmp(str, "gpe") == 0)
		app_config->vxlan_type = DOCA_FLOW_TUN_EXT_VXLAN_GPE;
	else if (strcmp(str, "vxlan") == 0)
		app_config->vxlan_type = DOCA_FLOW_TUN_EXT_VXLAN_STANDARD;
	else {
		DOCA_LOG_ERR("Unknown vxlan_type '%s' was specified", str);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	return DOCA_SUCCESS;
}

/*
 * Conversion function from a user context to the flow_dev_ctx struct
 *
 * @param [in]: User context
 * @return: Pointer to the flow_dev_ctx struct
 */
static struct flow_dev_ctx *flow_dev_ctx_from_user_ctx(void *user_ctx)
{
	return &((struct vxlan_encap_config *)user_ctx)->flow_dev_ctx;
}

/*
 * Register for vxlan_encap params
 *
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
static int register_vxlan_type_params(void)
{
	doca_error_t result;
	struct doca_argp_param *vxlan_type_param;

	result = register_flow_device_params(flow_dev_ctx_from_user_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register flow device params: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register vxlan-type para */
	result = doca_argp_param_create(&vxlan_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create vxlan-type ARGP param: %s", doca_error_get_descr(result));
		return result;
	}

	doca_argp_param_set_short_name(vxlan_type_param, "type");
	doca_argp_param_set_long_name(vxlan_type_param, "vxlan-type");
	doca_argp_param_set_arguments(vxlan_type_param, "<vxlan-type>");
	doca_argp_param_set_description(vxlan_type_param,
					"Set vxlan-type (\"gpe\", \"gbp\", \"vxlan\") to create encap flows");
	doca_argp_param_set_callback(vxlan_type_param, vxlan_type_callback);
	doca_argp_param_set_type(vxlan_type_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(vxlan_type_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return 0;
}

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;
	struct vxlan_encap_config app_cfg = {};
	struct application_dpdk_config dpdk_config = {
		.port_config.nb_ports = 2,
		.port_config.nb_queues = 4,
		.reserve_main_thread = true,
	};
	app_cfg.vxlan_type = DOCA_FLOW_TUN_EXT_VXLAN_STANDARD;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	result = doca_argp_init(NULL, &app_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_vxlan_type_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	doca_argp_set_dpdk_program(flow_init_dpdk);
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = init_doca_flow_devs(&app_cfg.flow_dev_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init flow devices: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	/* update queues and ports */
	result = dpdk_queues_and_ports_init(&dpdk_config);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to update ports and queues");
		goto dpdk_cleanup;
	}

	/* run sample */
	result = flow_vxlan_encap(dpdk_config.port_config.nb_queues, app_cfg.vxlan_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("flow_vxlan_encap() encountered an error: %s", doca_error_get_descr(result));
		goto dpdk_ports_queues_cleanup;
	}

	exit_status = EXIT_SUCCESS;

dpdk_ports_queues_cleanup:
	dpdk_queues_and_ports_fini(&dpdk_config);
dpdk_cleanup:
	dpdk_fini_with_devs(dpdk_config.port_config.nb_ports);
argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
