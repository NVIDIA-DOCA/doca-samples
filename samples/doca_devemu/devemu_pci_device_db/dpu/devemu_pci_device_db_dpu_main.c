/*
 * Copyright (c) 2024-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <string.h>

#include <doca_argp.h>
#include <doca_config.h>
#include <doca_error.h>
#include <doca_dev.h>
#include <doca_log.h>

#include <devemu_pci_common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_DB_DPU::MAIN);

/* Sample's Logic */
doca_error_t devemu_pci_device_db_dpu(const char *pci_address,
				      const char *emulated_dev_vuid,
				      uint16_t db_region_idx,
				      uint32_t db_id,
				      bool db_on_dpu);

/* Configuration struct */
struct devemu_pci_cfg {
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* device PCI address */
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE];	      /* VUID of emulated device with DB regions */
	uint16_t db_region_idx;			      /* Index of DB region to listen on */
	uint32_t db_id;				      /* The DB ID of the DB to listen on */
	bool db_on_dpu;				      /* Listen on DBs from DPU (true) or DPA (false) */
};

#ifdef DOCA_ARCH_DPU

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	const char *addr = (char *)param;

	return parse_pci_address(addr, conf->pci_address);
}

/*
 * ARGP Callback - Handle VUID parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t vuid_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	const char *vuid = (char *)param;

	return parse_vuid(vuid, conf->vuid);
}

/*
 * ARGP Callback - Handle DB region index parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t region_index_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	int db_region_idx = *(int *)param;

	if (db_region_idx < 0 || db_region_idx >= PCI_TYPE_NUM_BAR_DB_REGIONS) {
		DOCA_LOG_ERR(
			"Entered DB region index must be in [0, %u (PCI_TYPE_NUM_BAR_DB_REGIONS)) but received %d instead",
			PCI_TYPE_NUM_BAR_DB_REGIONS,
			db_region_idx);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->db_region_idx = (uint16_t)db_region_idx;

	return DOCA_SUCCESS;
}

/*
 * Register DB region index command line parameter
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_region_index_param(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "r");
	doca_argp_param_set_long_name(param, "region-index");
	doca_argp_param_set_description(param,
					"The index of the DB region as defined in devemu_pci_type_config.h. Integer");
	doca_argp_param_set_callback(param, region_index_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle DB ID parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t db_id_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;
	int db_id = *(int *)param;

	if (db_id < 0) {
		DOCA_LOG_ERR("Entered DB ID must be greater than 0 but received %d instead", db_id);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->db_id = (uint32_t)db_id;

	return DOCA_SUCCESS;
}

/*
 * Register DB ID command line parameter
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_db_id_param(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, "i");
	doca_argp_param_set_long_name(param, "db-id");
	doca_argp_param_set_description(
		param,
		"The DB ID of the DB. Sample will listen on DBs related to this DB ID. Integer");
	doca_argp_param_set_callback(param, db_id_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle DB datapath flag parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t db_on_dpu_callback(void *param, void *config)
{
	struct devemu_pci_cfg *conf = (struct devemu_pci_cfg *)config;

	conf->db_on_dpu = *(bool *)param;

	return DOCA_SUCCESS;
}

/*
 * Register DB datapath flag command line parameter
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_db_on_dpu_param(void)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_long_name(param, "db-on-dpu");
	doca_argp_param_set_description(
		param,
		"Listen on DBs from the DPU. If not set, DBs are listened on from the DPA (default)");
	doca_argp_param_set_callback(param, db_on_dpu_callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_devemu_pci_params(void)
{
	doca_error_t result;

	result = register_pci_address_param(pci_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_vuid_param(
		"DOCA Devemu emulated device VUID. Sample will use this device to handle Doorbells from Host",
		vuid_callback);
	if (result != DOCA_SUCCESS)
		return result;

	result = register_region_index_param();
	if (result != DOCA_SUCCESS)
		return result;

	result = register_db_id_param();
	if (result != DOCA_SUCCESS)
		return result;

	return register_db_on_dpu_param();
}

#endif // DOCA_ARCH_DPU

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	struct devemu_pci_cfg devemu_pci_cfg;
	doca_error_t result;
	struct doca_log_backend *sdk_log;
	int exit_status = EXIT_FAILURE;

	/* Set the default configuration values (Example values) */
	strcpy(devemu_pci_cfg.pci_address, "0000:03:00.0");
	strcpy(devemu_pci_cfg.vuid, "");
	devemu_pci_cfg.db_region_idx = 0;
	devemu_pci_cfg.db_id = 0;
	devemu_pci_cfg.db_on_dpu = false;

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

#ifdef DOCA_ARCH_DPU
	result = doca_argp_init(NULL, &devemu_pci_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}
	result = register_devemu_pci_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register sample command line parameters: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}
	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	if (*devemu_pci_cfg.vuid == 0) {
		DOCA_LOG_ERR("The VUID parameter is missing");
		goto argp_cleanup;
	}

	result = devemu_pci_device_db_dpu(devemu_pci_cfg.pci_address,
					  devemu_pci_cfg.vuid,
					  devemu_pci_cfg.db_region_idx,
					  devemu_pci_cfg.db_id,
					  devemu_pci_cfg.db_on_dpu);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("devemu_pci_device_db_dpu() encountered an error: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();

#else // DOCA_ARCH_DPU
	(void)argc;
	(void)argv;

	DOCA_LOG_ERR("PCI Emulated Device DB DPU can run only on the DPU");
	exit_status = EXIT_FAILURE;

#endif // DOCA_ARCH_DPU

sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
