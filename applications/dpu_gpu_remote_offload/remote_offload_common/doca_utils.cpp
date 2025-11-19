/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <remote_offload_common/doca_utils.hpp>

#include <array>

#include <doca_error.h>

#include <remote_offload_common/runtime_error.hpp>

using namespace std::string_literals;

namespace remote_offload {

doca_dev *open_device(std::string const &identifier)
{
	static auto constexpr pci_addr_len = sizeof("XX:XX.X") - sizeof('\0');
	static auto constexpr pci_long_addr_len = sizeof("XXXX:XX:XX.X") - sizeof('\0');
	static auto constexpr max_name_length = std::max(DOCA_DEVINFO_IFACE_NAME_SIZE, DOCA_DEVINFO_IBDEV_NAME_SIZE);

	doca_error_t result;
	doca_devinfo **list = nullptr;
	uint32_t list_size = 0;
	result = doca_devinfo_create_list(&list, &list_size);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Unable to enumerate doca devices"};
	}

	doca_devinfo *selected_devinfo = nullptr;

	for (uint32_t ii = 0; ii != list_size; ++ii) {
		auto *devinfo = list[ii];
		std::array<char, max_name_length> device_name;

		if (identifier.size() == pci_addr_len || identifier.size() == pci_long_addr_len) {
			uint8_t is_addr_equal = 0;
			result = doca_devinfo_is_equal_pci_addr(devinfo, identifier.c_str(), &is_addr_equal);
			if (result == DOCA_SUCCESS && is_addr_equal) {
				selected_devinfo = devinfo;
				break;
			}
		}

		result = doca_devinfo_get_ibdev_name(devinfo, device_name.data(), device_name.size());
		if (result == DOCA_SUCCESS) {
			if (strcmp(identifier.c_str(), device_name.data()) == 0) {
				selected_devinfo = devinfo;
				break;
			}
		}

		result = doca_devinfo_get_iface_name(devinfo, device_name.data(), device_name.size());
		if (result == DOCA_SUCCESS) {
			if (strcmp(identifier.c_str(), device_name.data()) == 0) {
				selected_devinfo = devinfo;
				break;
			}
		}
	}

	if (selected_devinfo == nullptr) {
		static_cast<void>(doca_devinfo_destroy_list(list));
		throw remote_offload::runtime_error{DOCA_ERROR_NOT_FOUND,
						    "No doca device found that matched given identifier: \"" +
							    identifier + "\""};
	}

	doca_dev *opened_device;
	result = doca_dev_open(selected_devinfo, &opened_device);
	static_cast<void>(doca_devinfo_destroy_list(list));
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to open doca device"};
	}

	return opened_device;
}

doca_dev_rep *open_representor(doca_dev *dev, std::string const &identifier)
{
	doca_error_t result;
	doca_devinfo_rep **list = nullptr;
	uint32_t list_size = 0;

	uint8_t supports_net_filter = 0;
	result = doca_devinfo_rep_cap_is_filter_net_supported(doca_dev_as_devinfo(dev), &supports_net_filter);
	if (result != DOCA_SUCCESS || supports_net_filter == 0)
		throw remote_offload::runtime_error{result, "Selected doca device does not support representors"};

	result = doca_devinfo_rep_create_list(dev, DOCA_DEVINFO_REP_FILTER_NET, &list, &list_size);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Unable to enumerate doca device representors"};
	}

	for (uint32_t ii = 0; ii != list_size; ++ii) {
		auto *repinfo = list[ii];
		uint8_t is_addr_equal;

		result = doca_devinfo_rep_is_equal_pci_addr(repinfo, identifier.c_str(), &is_addr_equal);
		if (result == DOCA_SUCCESS && is_addr_equal) {
			doca_dev_rep *rep;

			result = doca_dev_rep_open(repinfo, &rep);
			if (result != DOCA_SUCCESS) {
				static_cast<void>(doca_devinfo_rep_destroy_list(list));
				throw remote_offload::runtime_error{result, "Unable to open doca device representor"};
			}

			static_cast<void>(doca_devinfo_rep_destroy_list(list));
			return rep;
		}
	}

	static_cast<void>(doca_devinfo_rep_destroy_list(list));
	throw remote_offload::runtime_error{DOCA_ERROR_NOT_FOUND,
					    "No doca device representor found that matched given identifier: \"" +
						    identifier + "\""};
}

doca_error_t stop_context(doca_ctx *ctx, doca_pe *pe, std::vector<doca_task *> &ctx_tasks) noexcept
{
	/* If all tasks are completed, they can be released immediately to avoid any complaints during stop */
	size_t num_inflight_tasks = 0;
	static_cast<void>(doca_ctx_get_num_inflight_tasks(ctx, &num_inflight_tasks));
	if (num_inflight_tasks == 0) {
		for (auto *task : ctx_tasks)
			doca_task_free(task);
		ctx_tasks.clear();
	}

	auto ret = doca_ctx_stop(ctx);
	if (ret == DOCA_SUCCESS)
		return DOCA_SUCCESS;

	if (ret != DOCA_ERROR_AGAIN && ret != DOCA_ERROR_IN_USE && ret != DOCA_ERROR_IN_PROGRESS)
		return ret;

	/* Submitted tasks require the context to start stopping to flush them out (via the error callback) before they
	 * can be released, progress the context until all pending tasks have been completed.
	 */
	do {
		static_cast<void>(doca_ctx_get_num_inflight_tasks(ctx, &num_inflight_tasks));
		for (size_t ii = 0; ii != num_inflight_tasks; ++ii)
			static_cast<void>(doca_pe_progress(pe));
	} while (num_inflight_tasks != 0);

	for (auto *task : ctx_tasks)
		doca_task_free(task);
	ctx_tasks.clear();

	/* In the case of having had in flight tasks the context may need more time to clean up after they have
	 * completed. Continue to progress the context until it returns to the idle state.
	 */
	for (;;) {
		static_cast<void>(doca_pe_progress(pe));
		doca_ctx_states cur_state = DOCA_CTX_STATE_IDLE;
		static_cast<void>(doca_ctx_get_state(ctx, &cur_state));
		if (cur_state == DOCA_CTX_STATE_IDLE) {
			return DOCA_SUCCESS;
		}
	}
}

} // namespace remote_offload
