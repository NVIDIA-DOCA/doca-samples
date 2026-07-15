/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_devemu_vfs_type.h>
#include <doca_devemu_vfs_dev.h>
#include <doca_devemu_vfs.h>

DOCA_LOG_REGISTER(DPU_DEVEMU_VFS_DEVICE_CREATE);

#define SLEEP_IN_MICROS (10) /* Sample the task every 10 microseconds */
#define SLEEP_IN_NANOS (SLEEP_IN_MICROS * 1000)
#define TIMEOUT_IN_MICROS (5 * 1000 * 1000) /* Set timeout to 5 seconds  */

#define VFS_DEVICE_QUEUE_SIZE 64
#define VFS_DEVICE_NUM_QUEUES 16

static const char tag[DOCA_VFS_TAG_SIZE + 1] = "virtiofs-sample-tag";

struct program_state {
	struct doca_pe *pe;		       /**< Progress engine to retrieve events */
	struct doca_dev *dev;		       /**< Device that manage the emulated device */
	struct doca_dev_rep *rep;	       /**< Representor of the emulated device */
	struct doca_ctx *ctx;		       /**< DOCA context representation of pci_dev */
	struct doca_devemu_vfs_dev *vfs_dev;   /**< VFS Device for hotplugged device */
	struct doca_devemu_vfs_type *vfs_type; /**< VFS Type of the emulated device */
};

static bool unplug_requested; /* Shared variable to allow for a proper shutdown */

/*
 * Signal handler
 *
 * @signum [in]: Signal number to handle
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		unplug_requested = true;
	}
}

/*
 * Cleanup state of the sample
 *
 * @state [in]: The state of the sample
 */
static void state_cleanup(struct program_state *state)
{
	doca_error_t res;

	if (state->ctx != NULL) {
		res = doca_ctx_stop(doca_devemu_vfs_dev_as_ctx(state->vfs_dev));
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to stop DOCA Emulated Device context: %s", doca_error_get_descr(res));
	}

	if (state->vfs_type != NULL && state->vfs_dev != NULL) {
		res = doca_devemu_vfs_dev_destroy(state->vfs_dev);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy vfs_dev: %s", doca_error_get_descr(res));
	}

	if (state->rep != NULL) {
		res = doca_devemu_pci_type_destroy_rep(state->rep);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA Emulated Device representor: %s", doca_error_get_descr(res));

		state->rep = NULL;
	}

	if (state->dev != NULL) {
		res = doca_dev_close(state->dev);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(res));

		state->dev = NULL;
	}

	if (state->pe != NULL) {
		res = doca_pe_destroy(state->pe);
		if (res != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy DOCA progress engine: %s", doca_error_get_descr(res));

		state->pe = NULL;
	}
}

/*
 * Check if the device's PCI address matches the provided PCI address
 * Supports both formats DOMAIN:BUS:DEVICE.FUNCTION or BUS:DEVICE.FUNCTION
 *
 * @devinfo [in]: The DOCA device information
 * @pci_address [in]: The PCI address to compare against
 * @return: true in case matches, false otherwise
 */
static bool device_has_pci_addr(const struct doca_devinfo *devinfo, const char *pci_address)
{
	uint8_t is_equal = 0;

	(void)doca_devinfo_is_equal_pci_addr(devinfo, pci_address, &is_equal);

	return is_equal == 1;
}

/*
 * Open a DOCA device according to a given PCI address
 * Picks device that has given PCI address and supports hotplug of the PCI type
 *
 * @pci_address [in]: PCI address
 * @dev [out]: pointer to doca_dev struct, NULL if not found
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t find_doca_device(const char *pci_address, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*dev = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		if (!device_has_pci_addr(dev_list[i], pci_address))
			continue;

		res = doca_dev_open(dev_list[i], dev);
		if (res == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			return res;
		}
	}

	DOCA_LOG_WARN("Matching device not found");

	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

/*
 * Callback that is triggered everytime the hotplug state is changed for the associated emulated PCI device
 *
 * @pci_dev [in]: The associated emulated PCI device
 * @user_data [in]: The user data that was previously provided along with callback
 */
static void hotplug_event_handler_cb(struct doca_devemu_pci_dev *pci_dev, union doca_data user_data)
{
	(void)user_data;

	enum doca_devemu_pci_hotplug_state hotplug_state;
	doca_error_t res;

	DOCA_LOG_INFO("Emulated device's hotplug state has changed");

	res = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &hotplug_state);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get hotplug state: %s", doca_error_get_descr(res));
		return;
	}

	switch (hotplug_state) {
	case DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF:
		DOCA_LOG_INFO("Hotplug state changed to DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF");
		break;
	case DOCA_DEVEMU_PCI_HP_STATE_UNPLUG_IN_PROGRESS:
		DOCA_LOG_INFO("Hotplug state changed to DOCA_DEVEMU_PCI_HP_STATE_UNPLUG_IN_PROGRESS");
		break;
	case DOCA_DEVEMU_PCI_HP_STATE_PLUG_IN_PROGRESS:
		DOCA_LOG_INFO("Hotplug state changed to DOCA_DEVEMU_PCI_HP_STATE_PLUG_IN_PROGRESS");
		break;
	case DOCA_DEVEMU_PCI_HP_STATE_POWER_ON:
		DOCA_LOG_INFO("Hotplug state changed to DOCA_DEVEMU_PCI_HP_STATE_POWER_ON");
		break;
	default:
		break;
	}
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
		DOCA_LOG_ERR("Unable to allocate emulated PCI device hotplug state change event: %s",
			     doca_error_get_descr(res));
		return res;
	}

	return DOCA_SUCCESS;
}

/*
 * Run DOCA Device Emulation Create sample
 *
 * @pci_address [in]: Device PCI address
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_vfs_device_create(const char *pci_address)
{
	doca_error_t result;
	uint8_t is_supported = 0;
	size_t elapsed_time_in_micros;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANOS,
	};
	struct program_state state = {0};

	/* Signal the while loop to stop and unplug the emulated device */
	unplug_requested = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	result = doca_pe_create(&state.pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(result));
		return result;
	}

	result = find_doca_device(pci_address, &state.dev);
	if (result != DOCA_SUCCESS) {
		state_cleanup(&state);
		return result;
	}

	result = doca_devemu_vfs_find_default_vfs_type_by_dev(state.dev, &state.vfs_type);
	if (result != DOCA_SUCCESS) {
		state_cleanup(&state);
		return result;
	}

	/* Check if hotplug supported */
	result = doca_devemu_pci_cap_type_is_hotplug_supported(doca_dev_as_devinfo(state.dev),
							       doca_devemu_vfs_type_as_pci_type(state.vfs_type),
							       &is_supported);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to check hotplug support: %s", doca_error_get_descr(result));
		state_cleanup(&state);
		return result;
	}

	if (is_supported == 0) {
		DOCA_LOG_WARN(
			"Found device with matching address, but does not have hotplug support. Make sure a physical function was provided, and running with root permission");
		state_cleanup(&state);
		return result;
	}

	/* Prepare emulated device before plugging it towards the host */
	result = doca_devemu_pci_type_create_rep(doca_devemu_vfs_type_as_pci_type(state.vfs_type), &state.rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI emulated device representor: %s", doca_error_get_descr(result));
		state_cleanup(&state);
		return result;
	}

	result = doca_devemu_vfs_dev_create(state.vfs_type, state.rep, state.pe, &state.vfs_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create VFS emulated device context: %s", doca_error_get_descr(result));
		state_cleanup(&state);
		return result;
	}

	doca_devemu_virtio_dev_set_queue_size(doca_devemu_vfs_dev_as_virtio_dev(state.vfs_dev), VFS_DEVICE_QUEUE_SIZE);
	doca_devemu_virtio_dev_set_num_queues(doca_devemu_vfs_dev_as_virtio_dev(state.vfs_dev), VFS_DEVICE_NUM_QUEUES);

	doca_devemu_vfs_dev_set_tag(state.vfs_dev, tag);
	doca_devemu_vfs_dev_set_num_request_queues(state.vfs_dev, VFS_DEVICE_NUM_QUEUES);

	result = doca_ctx_start(doca_devemu_vfs_dev_as_ctx(state.vfs_dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start PCI emulated device context: %s", doca_error_get_descr(result));
		state_cleanup(&state);
		return result;
	}

	/* Defer assignment so that cleanup does not stop the context in case it was not started */
	state.ctx = doca_devemu_vfs_dev_as_ctx(state.vfs_dev);

	/* Register callback to be triggered once hotplug state changes */
	result = register_to_hotplug_state_change_events(doca_devemu_vfs_dev_as_pci_dev(state.vfs_dev), (void *)&state);
	if (result != DOCA_SUCCESS) {
		state_cleanup(&state);
		return result;
	}

	result = doca_devemu_pci_dev_hotplug(doca_devemu_vfs_dev_as_pci_dev(state.vfs_dev));
	if (result != DOCA_SUCCESS) {
		state_cleanup(&state);
		return result;
	}

	DOCA_LOG_INFO("Hotplug initiated waiting for host to notice new device");
	DOCA_LOG_INFO("Press ([ctrl] + c) to unplug device");

	/* Wait for emulated device to be hotplugged */
	while (doca_pe_progress(state.pe) == 0) {
		nanosleep(&ts, &ts);
		if (unplug_requested)
			break;
	}

	while (!unplug_requested)
		nanosleep(&ts, &ts);

	result = doca_devemu_pci_dev_hotunplug(doca_devemu_vfs_dev_as_pci_dev(state.vfs_dev));
	if (result != DOCA_SUCCESS) {
		state_cleanup(&state);
		return result;
	}

	DOCA_LOG_INFO("Hotunplug initiated waiting for host to release device");

	/* Wait for emulated device to be unplugged */
	elapsed_time_in_micros = 0;
	while (doca_pe_progress(state.pe) == 0 && elapsed_time_in_micros < TIMEOUT_IN_MICROS) {
		nanosleep(&ts, &ts);
		elapsed_time_in_micros += SLEEP_IN_MICROS;
	}

	/* Clean and destroy all relevant objects */
	state_cleanup(&state);

	return result;
}
