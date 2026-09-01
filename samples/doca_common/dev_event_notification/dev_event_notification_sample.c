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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_types.h>

DOCA_LOG_REGISTER(DEV_EVENT_NOTIFICATION::SAMPLE);

#define DEV_EVENT_NOTIFICATION_SAMPLE_EPOLL_MAX_EVENTS 1
#define DEV_EVENT_NOTIFICATION_SAMPLE_EPOLL_WAIT_TIMEOUT_MS 1000

#define GOTO_ON_FAILURE(_expression_) \
	do { \
		result = (_expression_); \
		if (result != DOCA_SUCCESS) { \
			DOCA_LOG_ERR("%s failed with status %s", #_expression_, doca_error_get_descr(result)); \
			goto cleanup; \
		} \
	} while (0)

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int signo)
{
	(void)signo;
	g_running = 0;
}

static const char *link_state_to_str(enum doca_dev_link_state state)
{
	switch (state) {
	case DOCA_DEV_LINK_STATE_UP:
		return "UP";
	case DOCA_DEV_LINK_STATE_DOWN:
		return "DOWN";
	default:
		return "UNKNOWN";
	}
}

static const char *link_event_type_to_str(enum doca_dev_event_type event_type)
{
	switch (event_type) {
	case DOCA_DEV_EVENT_TYPE_LINK_UP:
		return "UP";
	case DOCA_DEV_EVENT_TYPE_LINK_DOWN:
		return "DOWN";
	default:
		return "UNKNOWN";
	}
}

static void on_dev_event(struct doca_dev *dev, enum doca_dev_event_type event_type, union doca_data user_data)
{
	(void)user_data;

	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	doca_error_t ret = doca_devinfo_get_ibdev_name(doca_dev_as_devinfo(dev), ibdev_name, sizeof(ibdev_name));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get IB device name: %s", doca_error_get_name(ret));
		return;
	}
	DOCA_LOG_INFO("IB device %s link state changed to %s", ibdev_name, link_event_type_to_str(event_type));
}

/**
 * @brief Run the device event notification sample.
 *
 * Opens the first available device, verifies link up/down event notification support,
 * registers a callback for those events, connects the internal event notification ctx to a PE,
 * and waits for link events using an epoll-driven PE loop.
 */
doca_error_t run_dev_event_notification_sample(void)
{
	struct doca_devinfo **dev_list = NULL;
	struct doca_dev *dev = NULL;
	struct doca_pe *pe = NULL;
	struct doca_ctx *event_notification_ctx = NULL;
	doca_notification_handle_t pe_handle = doca_event_invalid_handle;
	struct epoll_event ev = {0};
	uint32_t nb_devs = 0;
	uint64_t supported_events = 0;
	const uint64_t link_events = DOCA_DEV_EVENT_TYPE_LINK_DOWN | DOCA_DEV_EVENT_TYPE_LINK_UP;
	enum doca_dev_link_state link_state = DOCA_DEV_LINK_STATE_DOWN;
	int epfd = -1;
	int epoll_status = 0;
	union doca_data user_data = {.u64 = 0};
	struct sigaction sa = {0};
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t cleanup_status = DOCA_SUCCESS;
	bool event_notification_registered = false;
	bool event_notification_ctx_started = false;

	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
		DOCA_LOG_ERR("Failed to install signal handlers, errno=%d", errno);
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	GOTO_ON_FAILURE(doca_devinfo_create_list(&dev_list, &nb_devs));
	if (nb_devs == 0) {
		DOCA_LOG_ERR("No DOCA devices found");
		result = DOCA_ERROR_NOT_FOUND;
		goto cleanup;
	}

	GOTO_ON_FAILURE(doca_devinfo_cap_is_event_notification_supported(dev_list[0], &supported_events));
	if ((supported_events & link_events) != link_events) {
		DOCA_LOG_ERR("Device does not support required link up/down event notifications");
		result = DOCA_ERROR_NOT_SUPPORTED;
		goto cleanup;
	}

	GOTO_ON_FAILURE(doca_dev_open(dev_list[0], &dev));
	result = doca_devinfo_destroy_list(dev_list);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("doca_devinfo_destroy_list(dev_list) failed with status %s", doca_error_get_descr(result));
		goto cleanup;
	}
	dev_list = NULL;

	GOTO_ON_FAILURE(doca_pe_create(&pe));
	GOTO_ON_FAILURE(doca_dev_register_event_notification(dev, on_dev_event, link_events, user_data));
	event_notification_registered = true;

	event_notification_ctx = doca_dev_as_ctx(dev);
	if (event_notification_ctx == NULL) {
		DOCA_LOG_ERR("Failed to get event notification ctx from device");
		result = DOCA_ERROR_UNEXPECTED;
		goto cleanup;
	}

	GOTO_ON_FAILURE(doca_pe_connect_ctx(pe, event_notification_ctx));
	GOTO_ON_FAILURE(doca_ctx_start(event_notification_ctx));
	event_notification_ctx_started = true;
	GOTO_ON_FAILURE(doca_dev_get_link_state(dev, &link_state));
	DOCA_LOG_INFO("Initial link state is %s", link_state_to_str(link_state));
	DOCA_LOG_INFO("Monitoring device events (press Ctrl+C to stop)");

	GOTO_ON_FAILURE(doca_pe_get_notification_handle(pe, &pe_handle));
	epfd = epoll_create1(0);
	if (epfd < 0) {
		DOCA_LOG_ERR("epoll_create1 failed, errno=%d", errno);
		result = DOCA_ERROR_OPERATING_SYSTEM;
		goto cleanup;
	}

	ev.events = EPOLLIN;
	ev.data.fd = pe_handle;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, pe_handle, &ev) != 0) {
		DOCA_LOG_ERR("epoll_ctl failed, errno=%d", errno);
		result = DOCA_ERROR_OPERATING_SYSTEM;
		goto cleanup;
	}

	while (g_running) {
		GOTO_ON_FAILURE(doca_pe_request_notification(pe));
		epoll_status = epoll_wait(epfd,
					  &ev,
					  DEV_EVENT_NOTIFICATION_SAMPLE_EPOLL_MAX_EVENTS,
					  DEV_EVENT_NOTIFICATION_SAMPLE_EPOLL_WAIT_TIMEOUT_MS);
		if (epoll_status == -1) {
			if (errno == EINTR) {
				continue;
			}
			DOCA_LOG_ERR("epoll_wait failed, errno=%d", errno);
			result = DOCA_ERROR_OPERATING_SYSTEM;
			goto cleanup;
		}
		if (epoll_status == 0) {
			continue;
		}

		GOTO_ON_FAILURE(doca_pe_clear_notification(pe, pe_handle));
		while (doca_pe_progress(pe)) {
			/* on_dev_event() may run here */
		}
	}

cleanup:
	if (epfd >= 0) {
		close(epfd);
		epfd = -1;
	}
	if (event_notification_ctx_started && event_notification_ctx != NULL) {
		cleanup_status = doca_ctx_stop(event_notification_ctx);
		if (cleanup_status != DOCA_SUCCESS && result == DOCA_SUCCESS) {
			result = cleanup_status;
		}
	}
	if (event_notification_registered && dev != NULL) {
		cleanup_status = doca_dev_unregister_event_notification(dev);
		if (cleanup_status != DOCA_SUCCESS && result == DOCA_SUCCESS) {
			result = cleanup_status;
		}
		event_notification_ctx = NULL;
	}
	if (pe != NULL) {
		cleanup_status = doca_pe_destroy(pe);
		if (cleanup_status != DOCA_SUCCESS && result == DOCA_SUCCESS) {
			result = cleanup_status;
		}
		pe = NULL;
	}
	if (dev != NULL) {
		cleanup_status = doca_dev_close(dev);
		if (cleanup_status != DOCA_SUCCESS && result == DOCA_SUCCESS) {
			result = cleanup_status;
		}
		dev = NULL;
	}
	if (dev_list != NULL) {
		cleanup_status = doca_devinfo_destroy_list(dev_list);
		if (cleanup_status != DOCA_SUCCESS) {
			DOCA_LOG_ERR("doca_devinfo_destroy_list(dev_list) failed with status %s",
				     doca_error_get_descr(cleanup_status));
		}
		dev_list = NULL;
	}

	return result;
}
