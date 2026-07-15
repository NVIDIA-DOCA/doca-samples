/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_devemu_pci.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_dpa.h>

#include <common.h>
#include <devemu_pci_common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_DB_DPU);

/*
 * A struct that includes all needed info on registered kernels and is initialized during linkage by DPACC.
 * Variable name should be the token passed to DPACC with --app-name parameter.
 */
extern struct doca_dpa_app *devemu_pci_sample_app;

/**
 * DPA RPC declaration
 */
extern doca_dpa_func_t init_app_ctx_rpc;

/**
 * DPA RPC declaration
 */
extern doca_dpa_func_t uninit_app_ctx_rpc;
/**
 * DB DPA handler declaration
 */
extern doca_dpa_func_t db_handler;

static bool force_quit; /* Shared variable to allow for a proper shutdown */

/*
 * Signal handler
 *
 * @signum [in]: Signal number to handle
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		force_quit = true;
	}
}

/*
 * Create a DPA thread that can be used to listen on DBs
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t init_dpa_db_thread(struct devemu_resources *resources)
{
	doca_error_t ret = doca_dpa_thread_create(resources->dpa, &resources->dpa_thread);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA thread: %s", doca_error_get_descr(ret));
		return ret;
	}

	ret = doca_dpa_thread_set_func_arg(resources->dpa_thread, db_handler, 0);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA thread func arg: %s", doca_error_get_descr(ret));
		return ret;
	}

	ret = doca_dpa_thread_start(resources->dpa_thread);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DPA thread: %s", doca_error_get_descr(ret));
		return ret;
	}

	return DOCA_SUCCESS;
}

/*
 * Create a DB DPA completion context
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_db_dpa_comp(struct devemu_resources *resources)
{
	doca_error_t result;

	result = doca_devemu_pci_db_completion_create(resources->dpa_thread, &resources->db_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DB completion context to be used in DPA: %s",
			     doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_db_completion_start(resources->db_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DB completion context: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_db_completion_get_dpa_handle(resources->db_comp, &resources->db_comp_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DB completion context DPA handle: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Initialize DPA app context
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t init_dpa_app_ctx(struct devemu_resources *resources)
{
	doca_error_t result;
	uint64_t rpc_ret;
	result = doca_dpa_rpc(resources->dpa,
			      &init_app_ctx_rpc,
			      &rpc_ret,
			      resources->db_comp_handle,
			      resources->data_path.db_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send RPC to initialize DPA app context: %s", doca_error_get_descr(result));
		return result;
	}
	if (rpc_ret != 0) {
		DOCA_LOG_ERR("RPC to initialize DPA app context has failed");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Uninitialize DPA app context
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t uninit_dpa_app_ctx(struct devemu_resources *resources)
{
	doca_error_t result;
	uint64_t rpc_ret;
	result = doca_dpa_rpc(resources->dpa,
			      &uninit_app_ctx_rpc,
			      &rpc_ret,
			      resources->db_comp_handle,
			      resources->data_path.db_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send RPC to uninitialize DPA app context: %s", doca_error_get_descr(result));
		return result;
	}
	if (rpc_ret != 0) {
		DOCA_LOG_ERR("RPC to uninitialize DPA app context has failed");
		return DOCA_ERROR_INVALID_VALUE;
	}

	return DOCA_SUCCESS;
}

/*
 * Create a DB object for the given resource ID and initialize it on DPA
 *
 * @resources [in]: The sample resources
 * @db_region_idx [in]: Index of the configured DB region
 * @db_id [in]: The DB ID of the DB
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t create_db_object(struct devemu_resources *resources, uint16_t db_region_idx, uint32_t db_id)
{
	doca_error_t result;

	const struct bar_db_region_config *db_region_cfg = &db_configs[db_region_idx];

	result = doca_devemu_pci_db_create_on_dpa(resources->pci_dev,
						  resources->db_comp,
						  db_region_cfg->region.bar_id,
						  db_region_cfg->region.start_address,
						  db_id,
						  0,
						  &resources->data_path.db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DB for use in DPA: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_db_get_dpa_handle(resources->data_path.db, &resources->data_path.db_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DB DPA handle: %s", doca_error_get_descr(result));
		return result;
	}

	/* To prevent DBs from reaching DB completion before binding, need to first bind the DB and then start the DB */
	result = init_dpa_app_ctx(resources);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_devemu_pci_db_start(resources->data_path.db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DB: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy the DB object and uninitialize it on DPA
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t destroy_db_object(struct devemu_resources *resources)
{
	doca_error_t result;

	result = doca_devemu_pci_db_stop(resources->data_path.db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DB: %s", doca_error_get_descr(result));
		return result;
	}

	result = uninit_dpa_app_ctx(resources);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_devemu_pci_db_destroy(resources->data_path.db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DB: %s", doca_error_get_descr(result));
		return result;
	}
	resources->data_path.db = NULL;

	return DOCA_SUCCESS;
}

/*
 * PCI FLR event handler callback
 *
 * @pci_dev [in]: The PCI device affected by the FLR
 * @user_data [in]: The same user_data that was provided on registration
 */
static void flr_event_handler_cb(struct doca_devemu_pci_dev *pci_dev, union doca_data user_data)
{
	struct devemu_resources *resources = (struct devemu_resources *)user_data.ptr;

	DOCA_LOG_INFO("FLR has occurred destroying PCI device and recreating it");
	doca_error_t result = destroy_db_object(resources);
	if (result != DOCA_SUCCESS)
		goto abort;

	result = doca_ctx_stop(doca_devemu_pci_dev_as_ctx(pci_dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop PCI device during FLR event");
		goto abort;
	}

	/* During FLR the context may transition to starting. DB recreation is deferred until context is running */
	result = doca_ctx_start(doca_devemu_pci_dev_as_ctx(pci_dev));
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start PCI device during FLR event");
		goto abort;
	}

	return;
abort:
	resources->error = result;
	force_quit = true;
}

/*
 * Register to PCI Function Level Reset events
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_to_flr_events(struct devemu_resources *resources)
{
	union doca_data user_data;
	doca_error_t res;

	user_data.ptr = (void *)resources;
	res = doca_devemu_pci_dev_event_flr_register(resources->pci_dev, flr_event_handler_cb, user_data);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register to FLR event: %s", doca_error_get_descr(res));
		return res;
	}

	return DOCA_SUCCESS;
}

/*
 * Context state change event handler callback
 *
 * @user_data [in]: The user_data associated with the context
 * @ctx [in]: The PCI device context that went through state change
 * @prev_state [in]: The state of the context before the change
 * @next_state [in]: The state of the context after the change
 */
static void state_change_event_handler_cb(const union doca_data user_data,
					  struct doca_ctx *ctx,
					  enum doca_ctx_states prev_state,
					  enum doca_ctx_states next_state)
{
	(void)prev_state;
	(void)ctx;

	doca_error_t result;
	struct devemu_resources *resources = (struct devemu_resources *)user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
	case DOCA_CTX_STATE_STARTING:
		break;
	case DOCA_CTX_STATE_RUNNING:
		result = create_db_object(resources, resources->db_region_idx, resources->db_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_INFO("Failed to create DB object: %s", doca_error_get_descr(result));
			resources->error = result;
			force_quit = true;
		}
		break;
	case DOCA_CTX_STATE_STOPPING:
		DOCA_LOG_ERR(
			"Devemu device has entered into stopping state. This happens only when attempting to stop before destroying doorbell");
		destroy_db_object(resources);
		break;
	default:
		break;
	}
}

/*
 * Register to context state change events
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_to_ctx_state_change_events(struct devemu_resources *resources)
{
	union doca_data user_data;
	doca_error_t result;

	struct doca_ctx *ctx = doca_devemu_pci_dev_as_ctx(resources->pci_dev);

	user_data.ptr = (void *)resources;
	result = doca_ctx_set_user_data(ctx, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set ctx user data: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_ctx_set_state_changed_cb(ctx, state_change_event_handler_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register to context state change event: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Run DOCA Device Emulation DB DPU sample
 *
 * @pci_address [in]: Device PCI address
 * @emulated_dev_vuid [in]: VUID of the emulated device
 * @db_region_idx [in]: Index of the DB region
 * @db_id [in]: DB ID of the DB
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_db_dpu(const char *pci_address,
				      const char *emulated_dev_vuid,
				      uint16_t db_region_idx,
				      uint32_t db_id)
{
	doca_error_t result;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	struct devemu_resources resources = {0};
	const char pci_type_name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN] = PCI_TYPE_NAME;
	bool destroy_rep = false;
	uint16_t max_num_db = 0;

	resources.error = DOCA_SUCCESS;
	resources.db_region_idx = db_region_idx;
	resources.db_id = db_id;

	/* Signal the while loop to stop */
	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	result = doca_pe_create(&resources.pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_pci_type_create(pci_type_name, &resources.pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI type: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = find_supported_device(pci_address,
				       resources.pci_type,
				       doca_devemu_pci_cap_type_is_hotplug_supported,
				       &resources.dev);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Check that DB ID is supported */
	result = doca_devemu_pci_cap_type_get_max_num_db(doca_dev_as_devinfo(resources.dev), &max_num_db);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		DOCA_LOG_ERR("Unable to get max num DBs per PCI type: %s", doca_error_get_descr(result));
		return result;
	}
	if (max_num_db == 0) {
		devemu_resources_cleanup(&resources, destroy_rep);
		DOCA_LOG_ERR("Device does not support DBs");
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	if (db_id >= (uint32_t)max_num_db) {
		devemu_resources_cleanup(&resources, destroy_rep);
		DOCA_LOG_ERR(
			"DB ID must not exceed the maximum number of DBs per PCI type: expected DB ID less than %u but received %u",
			max_num_db,
			db_id);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* Set PCIe configuration space values */
	result = configure_and_start_pci_type(resources.pci_type, resources.dev);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Initialize DPA context */
	result = init_dpa(&resources, devemu_pci_sample_app);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = init_dpa_db_thread(&resources);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = create_db_dpa_comp(&resources);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = doca_dpa_thread_run(resources.dpa_thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to run DPA thread: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Find existing emulated device */
	result = find_emulated_device(resources.pci_type, emulated_dev_vuid, &resources.rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to find PCI emulated device representor: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Create emulated device context */
	result = doca_devemu_pci_dev_create(resources.pci_type, resources.rep, resources.pe, &resources.pci_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI emulated device context: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = doca_ctx_set_datapath_on_dpa(doca_devemu_pci_dev_as_ctx(resources.pci_dev), resources.dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PCI emulated device context datapath on DPA: %s",
			     doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = register_to_ctx_state_change_events(&resources);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = register_to_flr_events(&resources);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	result = doca_ctx_start(doca_devemu_pci_dev_as_ctx(resources.pci_dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start PCI emulated device context: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Defer assignment so that cleanup does not stop the context in case it was not started */
	resources.ctx = doca_devemu_pci_dev_as_ctx(resources.pci_dev);

	result = doca_devemu_pci_dev_get_hotplug_state(resources.pci_dev, &resources.hotplug_state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get hotplug state: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	if (resources.hotplug_state != DOCA_DEVEMU_PCI_HP_STATE_POWER_ON) {
		DOCA_LOG_ERR(
			"Expected hotplug state to be DOCA_DEVEMU_PCI_HP_STATE_POWER_ON instead current state is %s",
			hotplug_state_to_string(resources.hotplug_state));
		devemu_resources_cleanup(&resources, destroy_rep);
		return DOCA_ERROR_BAD_STATE;
	}

	DOCA_LOG_INFO("Listening on DB with ID %u", db_id);

	/* Listen on FLR events while DPA listens on DBs */
	while (!force_quit) {
		if (doca_pe_progress(resources.pe) == 0)
			nanosleep(&ts, &ts);
	}

	if (resources.error != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Some error occurred during progress: %s", doca_error_get_descr(resources.error));
		devemu_resources_cleanup(&resources, destroy_rep);
		return resources.error;
	}

	result = destroy_db_object(&resources);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Clean and destroy all relevant objects */
	devemu_resources_cleanup(&resources, destroy_rep);

	return result;
}
