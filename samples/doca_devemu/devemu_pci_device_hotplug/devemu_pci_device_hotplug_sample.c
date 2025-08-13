/*
 * Copyright (c) 2023-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

DOCA_LOG_REGISTER(DPU_DEVEMU_PCI_DEVICE_HOTPLUG);

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
 * Callback that is triggered everytime the hotplug state is changed for the associated emulated PCI device
 *
 * @pci_dev [in]: The associated emulated PCI device
 * @user_data [in]: The user data that was previously provided along with callback
 */
static void hotplug_event_handler_cb(struct doca_devemu_pci_dev *pci_dev, union doca_data user_data)
{
	enum doca_devemu_pci_hotplug_state hotplug_state;
	doca_error_t res;
	struct devemu_resources *resources = (struct devemu_resources *)user_data.ptr;

	DOCA_LOG_INFO("Emulated device's hotplug state has changed");

	res = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &hotplug_state);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get hotplug state: %s", doca_error_get_descr(res));
		return;
	}

	resources->hotplug_state = hotplug_state;

	DOCA_LOG_INFO("Hotplug state changed to %s", hotplug_state_to_string(hotplug_state));
}

/*
 * Register to the hotplug state change event of the emulated device
 * After this the sample will be able to receive notification once the hotplug state of the emulated device
 * has been changed. For possible states check out enum doca_devemu_pci_hotplug_state
 *
 * @pci_dev [in]: The emulated device context
 * @cookie [in]: User data that is associated with the event, can be retrieved from event once callback is triggered
 * @event [out]: The newly registered event object
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_to_hotplug_state_change_events(struct doca_devemu_pci_dev *pci_dev, void *cookie)
{
	union doca_data user_data;
	doca_error_t res;

	user_data.ptr = cookie;
	res = doca_devemu_pci_dev_event_hotplug_state_change_register(pci_dev, hotplug_event_handler_cb, user_data);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to allocate emulated PCI device hotplug state change event : %s",
			     doca_error_get_descr(res));
		return res;
	}

	return DOCA_SUCCESS;
}

/*
 * Run DOCA Device Emulation Hotplug sample
 *
 * @pci_address [in]: Device PCI address
 * @unplug_vuid [in]: VUID of emulated device to unplug. Can be NULL
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_hotplug(const char *pci_address, const char *unplug_vuid)
{
	char rep_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	doca_error_t result;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	struct devemu_resources resources = {0};
	const char pci_type_name[DOCA_DEVEMU_PCI_TYPE_NAME_LEN] = PCI_TYPE_NAME;
	bool destroy_rep = false;
	enum doca_devemu_pci_hotplug_state expected_hotplug_state;

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

	/* Set PCIe configuration space values */
	result = configure_and_start_pci_type(resources.pci_type, resources.dev);
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* If unplug address was given then sample will hotunplug device */
	if (unplug_vuid != NULL) {
		/* Find existing emulated device */
		result = find_emulated_device(resources.pci_type, unplug_vuid, &resources.rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to find PCI emulated device representor: %s",
				     doca_error_get_descr(result));
			devemu_resources_cleanup(&resources, destroy_rep);
			return result;
		}
	} else {
		/* Prepare emulated device before plugging it towards the host */
		result = doca_devemu_pci_type_create_rep(resources.pci_type, &resources.rep);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create PCI emulated device representor: %s",
				     doca_error_get_descr(result));
			devemu_resources_cleanup(&resources, destroy_rep);
			return result;
		}

		/* Print VUID of new device */
		result = doca_devinfo_rep_get_vuid(doca_dev_rep_as_devinfo(resources.rep),
						   rep_vuid,
						   DOCA_DEVINFO_REP_VUID_SIZE);
		DOCA_LOG_INFO("The new emulated device VUID: %s",
			      result == DOCA_SUCCESS ? rep_vuid : doca_error_get_name(result));
	}

	/* Create emulated device context */
	result = doca_devemu_pci_dev_create(resources.pci_type, resources.rep, resources.pe, &resources.pci_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI emulated device context: %s", doca_error_get_descr(result));
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	/* Register callback to be triggered once hotplug state changes */
	result = register_to_hotplug_state_change_events(resources.pci_dev, (void *)&resources);
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

	DOCA_LOG_INFO("Current hotplug state of emulated device is %s",
		      hotplug_state_to_string(resources.hotplug_state));

	if (unplug_vuid != NULL) {
		result = doca_devemu_pci_dev_hotunplug(resources.pci_dev);
		expected_hotplug_state = DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF;
		DOCA_LOG_INFO("Hotunplug initiated waiting for host to release device");
	} else {
		result = doca_devemu_pci_dev_hotplug(resources.pci_dev);
		expected_hotplug_state = DOCA_DEVEMU_PCI_HP_STATE_POWER_ON;
		DOCA_LOG_INFO("Hotplug initiated waiting for host to notice new device");
	}
	if (result != DOCA_SUCCESS) {
		devemu_resources_cleanup(&resources, destroy_rep);
		return result;
	}

	DOCA_LOG_INFO("Press ([ctrl] + c) to force quit");

	/* Wait for emulated device to be hotplugged */
	while (resources.hotplug_state != expected_hotplug_state && !force_quit) {
		if (doca_pe_progress(resources.pe) == 0)
			nanosleep(&ts, &ts);
	}

	destroy_rep = (resources.hotplug_state == DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF);

	/* Clean and destroy all relevant objects */
	devemu_resources_cleanup(&resources, destroy_rep);

	return result;
}
