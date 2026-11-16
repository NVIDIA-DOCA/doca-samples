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
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_lifeguard_client.h>
#include <doca_log.h>

#include "common.h"

#include "lifeguard_deposit_sample.h"

DOCA_LOG_REGISTER(LIFEGUARD_DEPOSIT::SAMPLE);

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int signo)
{
	(void)signo;

	g_running = 0;
}

static doca_error_t install_signal_handlers(void)
{
	struct sigaction sa = {0};

	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
		DOCA_LOG_ERR("Failed to install signal handlers, errno=%d", errno);
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	return DOCA_SUCCESS;
}

/*
 * Release a deposited device, then close the client. Closing the client leaves
 * any deposit in place, so release has to happen first.
 */
static doca_error_t release_and_close(int lg_handle, struct doca_dev *dev, bool deposited)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp_result;

	if (deposited) {
		tmp_result = doca_lifeguard_release_doca_dev(lg_handle, dev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to release device: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	tmp_result = doca_lifeguard_close(lg_handle);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close lifeguard client: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t lifeguard_deposit(const struct lifeguard_deposit_cfg *cfg)
{
	struct doca_dev *dev = NULL;
	struct doca_dev *old_dev = NULL;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	int lg_handle = 0;
	bool connected = false;
	bool deposited = false;
	doca_error_t result;
	doca_error_t tmp_result;

	result = install_signal_handlers();
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_lifeguard_connect(cfg->socket_path, LIFEGUARD_DEPOSIT_SERVICE_NAME, &lg_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect to lifeguard at %s: %s",
			     cfg->socket_path,
			     doca_error_get_descr(result));
		DOCA_LOG_ERR("Start the service with: systemctl start doca_lifeguard.service");
		return result;
	}
	connected = true;
	DOCA_LOG_INFO("Connected to lifeguard as '%s'", LIFEGUARD_DEPOSIT_SERVICE_NAME);

	/* A colon marks a PCI address. Anything else is an interface name such as p0. */
	if (strchr(cfg->device, ':') != NULL)
		result = open_doca_device_with_pci(cfg->device, NULL, &dev);
	else
		result =
			open_doca_device_with_iface_name((const uint8_t *)cfg->device, strlen(cfg->device), NULL, &dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open DOCA device %s: %s", cfg->device, doca_error_get_descr(result));
		goto cleanup;
	}

	result = doca_devinfo_get_ibdev_name(doca_dev_as_devinfo(dev), ibdev_name, sizeof(ibdev_name));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get IB device name: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	DOCA_LOG_INFO("Using IB device %s", ibdev_name);

	/*
	 * Import hands this process the previous instance's cmd_fd. Release that
	 * record before depositing; a deposit while it still exists returns
	 * DOCA_ERROR_IN_USE. NULL is a cold start.
	 */
	old_dev = doca_lifeguard_import_doca_dev(lg_handle, ibdev_name);
	if (old_dev != NULL) {
		DOCA_LOG_INFO("Imported a previous deposit for %s", ibdev_name);
		result = doca_lifeguard_release_doca_dev(lg_handle, old_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to release imported device: %s", doca_error_get_descr(result));
			goto cleanup;
		}
	} else {
		DOCA_LOG_INFO("No previous deposit for %s", ibdev_name);
	}

	result = doca_lifeguard_deposit_doca_dev(lg_handle, dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to deposit device: %s", doca_error_get_descr(result));
		goto cleanup;
	}
	deposited = true;
	DOCA_LOG_INFO("Deposited %s with lifeguard", ibdev_name);

	/* Keep the imported device open until the new deposit exists. */
	if (old_dev != NULL) {
		tmp_result = doca_dev_close(old_dev);
		old_dev = NULL;
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close imported device: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			goto cleanup;
		}
	}

	DOCA_LOG_INFO("Holding the deposit until SIGINT or SIGTERM. SIGKILL leaves it for the next run");
	while (g_running)
		nanosleep(&(struct timespec){.tv_nsec = 100 * 1000 * 1000}, NULL);

	DOCA_LOG_INFO("Releasing deposit for %s", ibdev_name);

cleanup:
	if (connected) {
		tmp_result = release_and_close(lg_handle, dev, deposited);
		if (tmp_result != DOCA_SUCCESS)
			DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	if (old_dev != NULL) {
		tmp_result = doca_dev_close(old_dev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close imported device: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	if (dev != NULL) {
		tmp_result = doca_dev_close(dev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}

	return result;
}
