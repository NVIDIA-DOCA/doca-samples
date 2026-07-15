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

#include <devemu_pci_common.h>

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
#include <common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_STATEFUL_REGION_DPU);

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
 * Callback that is triggered everytime the host writes to a stateful region
 *
 * @event [in]: The stateful region write event
 * @user_data [in]: The user data that was provided along with callback during registration
 */
static void stateful_region_write_event_handler_cb(
	struct doca_devemu_pci_dev_event_bar_stateful_region_driver_write *event,
	union doca_data user_data)
{
	struct doca_devemu_pci_dev *pci_dev;
	union doca_data ctx_user_data;
	struct devemu_resources *resources;
	doca_error_t res;
	const struct bar_region_config *config = (const struct bar_region_config *)user_data.ptr;

	DOCA_LOG_INFO("Host wrote to stateful region of emulated device");

	pci_dev = doca_devemu_pci_dev_event_bar_stateful_region_driver_write_get_pci_dev(event);

	res = doca_ctx_get_user_data(doca_devemu_pci_dev_as_ctx(pci_dev), &ctx_user_data);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the context user data: %s", doca_error_get_descr(res));
		return;
	}

	resources = (struct devemu_resources *)ctx_user_data.ptr;
	res = doca_devemu_pci_dev_query_bar_stateful_region_values(pci_dev,
								   config->bar_id,
								   config->start_address,
								   resources->stateful_region_values,
								   config->size);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query values of stateful region: %s", doca_error_get_descr(res));
		return;
	}

	char *dump = hex_dump(resources->stateful_region_values, config->size);
	if (dump == NULL) {
		DOCA_LOG_ERR("Failed to dump values of stateful region: Memory allocation failure");
		return;
	}

	DOCA_LOG_INFO("Printing values of stateful region [bar_id=%u, start_address=%lu, size=%lu]\n%s",
		      config->bar_id,
		      config->start_address,
		      config->size,
		      dump);

	free(dump);
}

/*
 * Register to the stateful region write event of the emulated device for all stateful regions of configured type
 * After this the sample will be able to receive notification once the host writes to the stateful regions through the
 * bar of the emulated device.
 *
 * @pci_dev [in]: The emulated device context
 * @resources [in]: Sample resources, will be associated with context as user_data that can be fetched from context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_to_stateful_region_write_events(struct doca_devemu_pci_dev *pci_dev,
							     struct devemu_resources *resources)
{
	const struct bar_region_config *config;
	uint64_t region_idx;
	union doca_data user_data;
	doca_error_t res;
	uint64_t max_region_size = 0;

	for (region_idx = 0; region_idx < PCI_TYPE_NUM_BAR_STATEFUL_REGIONS; region_idx++) {
		config = &stateful_configs[region_idx];
		user_data.ptr = (void *)config;
		res = doca_devemu_pci_dev_event_bar_stateful_region_driver_write_register(
			pci_dev,
			stateful_region_write_event_handler_cb,
			config->bar_id,
			config->start_address,
			user_data);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to register to emulated PCI device stateful region write event: %s",
				     doca_error_get_descr(res));
			return res;
		}

		max_region_size = max_region_size > config->size ? max_region_size : config->size;
	}

	user_data.ptr = (void *)resources;
	res = doca_ctx_set_user_data(doca_devemu_pci_dev_as_ctx(pci_dev), user_data);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set context user data: %s", doca_error_get_descr(res));
		return res;
	}

	/* Setup a buffer that can be used to query stateful region values once event is triggered */
	resources->stateful_region_values = calloc(1, max_region_size);
	if (resources->stateful_region_values == NULL) {
		DOCA_LOG_ERR("Unable to allocate buffer for storing stateful region values: out of memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

/*
 * Run DOCA Device Emulation Stateful Region DPU sample
 *
 * @pci_address [in]: Device PCI address
 * @emulated_dev_vuid [in]: VUID of the emulated device
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_stateful_region_dpu(const char *pci_address, const char *emulated_dev_vuid)
{
	doca_error_t result;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	struct devemu_resources resources = {0};
	const char pci_type_name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN] = PCI_TYPE_NAME;
	bool destroy_rep = false;

	/* Signal the while loop to stop */
	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	if (PCI_TYPE_NUM_BAR_STATEFUL_REGIONS == 0) {
		DOCA_LOG_ERR(
			"No stateful region was configured for type. Please configure at least 1 stateful region to run this sample");
		return DOCA_ERROR_INVALID_VALUE;
	}

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

	/* Set PCIe configuration space values */
	result = configure_and_start_pci_type(resources.pci_type, resources.dev);
	if (result != DOCA_SUCCESS) {
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

	/* Register callback to be triggered once host writes to stateful regions */
	result = register_to_stateful_region_write_events(resources.pci_dev, &resources);
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

	DOCA_LOG_INFO("Press ([ctrl] + c) to stop sample");

	/* Listen to any writes to the emulated device's stateful regions */
	while (!force_quit) {
		if (doca_pe_progress(resources.pe) == 0)
			nanosleep(&ts, &ts);
	}

	/* Clean and destroy all relevant objects */
	devemu_resources_cleanup(&resources, destroy_rep);

	return result;
}
