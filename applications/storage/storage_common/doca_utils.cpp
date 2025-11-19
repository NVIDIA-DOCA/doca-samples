/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <storage_common/doca_utils.hpp>

#include <array>
#include <stdexcept>
#include <thread>

#include <doca_error.h>
#include <doca_log.h>

#include <storage_common/definitions.hpp>

DOCA_LOG_REGISTER(DOCA_UTILS);

using namespace std::string_literals;

namespace storage {

doca_dev *open_device(std::string const &identifier)
{
	static auto constexpr pci_addr_len = sizeof("XX:XX.X") - sizeof('\0');
	static auto constexpr pci_long_addr_len = sizeof("XXXX:XX:XX.X") - sizeof('\0');
	static auto constexpr max_name_length = std::max(DOCA_DEVINFO_IFACE_NAME_SIZE, IB_DEVICE_NAME_MAX);

	doca_error_t ret;
	doca_devinfo **list = nullptr;
	uint32_t list_size = 0;
	ret = doca_devinfo_create_list(&list, &list_size);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Unable to enumerate doca devices: "s + doca_error_get_name(ret)};
	}

	doca_devinfo *selected_devinfo = nullptr;

	for (uint32_t ii = 0; ii != list_size; ++ii) {
		auto *devinfo = list[ii];
		std::array<char, max_name_length> device_name;

		if (identifier.size() == pci_addr_len || identifier.size() == pci_long_addr_len) {
			uint8_t is_addr_equal = 0;
			ret = doca_devinfo_is_equal_pci_addr(devinfo, identifier.c_str(), &is_addr_equal);
			if (ret == DOCA_SUCCESS && is_addr_equal) {
				selected_devinfo = devinfo;
				break;
			}
		}

		ret = doca_devinfo_get_ibdev_name(devinfo, device_name.data(), device_name.size());
		if (ret == DOCA_SUCCESS) {
			if (strcmp(identifier.c_str(), device_name.data()) == 0) {
				selected_devinfo = devinfo;
				break;
			}
		}

		ret = doca_devinfo_get_iface_name(devinfo, device_name.data(), device_name.size());
		if (ret == DOCA_SUCCESS) {
			if (strcmp(identifier.c_str(), device_name.data()) == 0) {
				selected_devinfo = devinfo;
				break;
			}
		}
	};

	if (selected_devinfo == nullptr) {
		static_cast<void>(doca_devinfo_destroy_list(list));
		throw std::runtime_error{"No doca device found that matched given identifier: \"" + identifier + "\""};
	}

	doca_dev *opened_device;
	ret = doca_dev_open(selected_devinfo, &opened_device);
	static_cast<void>(doca_devinfo_destroy_list(list));
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to open doca device"s + doca_error_get_name(ret)};
	}

	return opened_device;
}

doca_dev_rep *open_representor(doca_dev *dev, std::string const &identifier)
{
	doca_error_t ret;
	doca_devinfo_rep **list = nullptr;
	uint32_t list_size = 0;

	uint8_t supports_net_filter = 0;
	ret = doca_devinfo_rep_cap_is_filter_net_supported(doca_dev_as_devinfo(dev), &supports_net_filter);
	if (ret != DOCA_SUCCESS || supports_net_filter == 0)
		throw std::runtime_error{"Selected doca device does not support representors"s +
					 doca_error_get_name(ret)};

	ret = doca_devinfo_rep_create_list(dev, DOCA_DEVINFO_REP_FILTER_NET, &list, &list_size);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Unable to enumerate doca device representors"s + doca_error_get_name(ret)};
	}

	for (uint32_t ii = 0; ii != list_size; ++ii) {
		auto *repinfo = list[ii];
		uint8_t is_addr_equal;

		ret = doca_devinfo_rep_is_equal_pci_addr(repinfo, identifier.c_str(), &is_addr_equal);
		if (ret == DOCA_SUCCESS && is_addr_equal) {
			doca_dev_rep *rep;

			ret = doca_dev_rep_open(repinfo, &rep);
			if (ret != DOCA_SUCCESS) {
				static_cast<void>(doca_devinfo_rep_destroy_list(list));
				throw std::runtime_error{"Unable to open doca device representor"s +
							 doca_error_get_name(ret)};
			}

			static_cast<void>(doca_devinfo_rep_destroy_list(list));
			return rep;
		}
	}

	static_cast<void>(doca_devinfo_rep_destroy_list(list));
	throw std::runtime_error{"No doca device representor found that matched given identifier: \"" + identifier +
				 "\""};
}

doca_mmap *make_mmap(doca_dev *dev,
		     char *memory_region,
		     size_t memory_region_size,
		     uint32_t permissions,
		     storage::thread_safety ts)
{
	doca_error_t ret;
	doca_mmap *obj = nullptr;
	ret = doca_mmap_create(&obj);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_mmap: "s + doca_error_get_name(ret)};
	}
	DOCA_LOG_DBG("Created mmap %p: dev: %p, region: %p, size: %lu, permissions: 0x%X",
		     obj,
		     dev,
		     memory_region,
		     memory_region_size,
		     permissions);

	ret = doca_mmap_add_dev(obj, dev);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_mmap_destroy(obj));
		throw std::runtime_error{"Failed to add doca_dev to doca_mmap: "s + doca_error_get_name(ret)};
	}

	ret = doca_mmap_set_memrange(obj, memory_region, memory_region_size);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_mmap_destroy(obj));
		throw std::runtime_error{"Failed to set doca_mmap memory range: "s + doca_error_get_name(ret)};
	}

	ret = doca_mmap_set_permissions(obj, permissions);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_mmap_destroy(obj));
		throw std::runtime_error{"Failed to set doca_mmap access permissions: "s + doca_error_get_name(ret)};
	}

	if (ts == thread_safety::yes) {
		ret = doca_mmap_enable_thread_safety(obj);
		if (ret != DOCA_SUCCESS) {
			static_cast<void>(doca_mmap_destroy(obj));
			throw std::runtime_error{"Failed to make doca_mmap thread safe: "s + doca_error_get_name(ret)};
		}
	}

	ret = doca_mmap_start(obj);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_mmap_destroy(obj));
		throw std::runtime_error{"Failed to start doca_mmap: "s + doca_error_get_name(ret)};
	}

	return obj;
}

doca_mmap *make_mmap(doca_dev *dev,
		     void const *mmap_export_blob,
		     size_t mmap_export_blob_size,
		     storage::thread_safety ts)
{
	doca_mmap *mmap = nullptr;
	auto ret = doca_mmap_create_from_export(nullptr, mmap_export_blob, mmap_export_blob_size, dev, &mmap);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_mmap from export: "s + doca_error_get_name(ret)};
	}

	if (ts == thread_safety::yes) {
		ret = doca_mmap_enable_thread_safety(mmap);
		if (ret != DOCA_SUCCESS) {
			static_cast<void>(doca_mmap_destroy(mmap));
			throw std::runtime_error{"Failed to make doca_mmap thread safe: "s + doca_error_get_name(ret)};
		}
	}

	DOCA_LOG_DBG("Created mmap from export: %p using: dev %p, blob %p, blob_size: %zu",
		     mmap,
		     dev,
		     mmap_export_blob,
		     mmap_export_blob_size);

	return mmap;
}

doca_buf_inventory *make_buf_inventory(size_t num_elements)
{
	doca_buf_inventory *inv;
	doca_error_t ret;

	ret = doca_buf_inventory_create(num_elements, &inv);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_buf_inventory: "s + doca_error_get_name(ret)};
	}

	ret = doca_buf_inventory_start(inv);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_buf_inventory_destroy(inv));
		throw std::runtime_error{"Failed to start doca_buf_inventory: "s + doca_error_get_name(ret)};
	}

	return inv;
}

doca_comch_consumer *make_comch_consumer(doca_comch_connection *conn,
					 doca_mmap *mmap,
					 doca_pe *pe,
					 uint32_t task_pool_size,
					 doca_data callback_user_data,
					 doca_comch_consumer_task_post_recv_completion_cb_t task_cb,
					 doca_comch_consumer_task_post_recv_completion_cb_t error_cb)
{
	doca_error_t ret;

	doca_comch_consumer *obj;
	ret = doca_comch_consumer_create(conn, mmap, &obj);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_comch_consumer: "s + doca_error_get_name(ret)};
	}

	DOCA_LOG_TRC(
		"Created consumer using; conn: %p, pe: %p, mmap: %p, task_pool_size: %u, callback_user_data: 0x%lX, task_cb: %p, error_cb: %p ",
		conn,
		pe,
		mmap,
		task_pool_size,
		callback_user_data.u64,
		task_cb,
		error_cb);

	ret = doca_pe_connect_ctx(pe, doca_comch_consumer_as_ctx(obj));
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_comch_consumer_destroy(obj));
		throw std::runtime_error{"Failed to connect doca_comch_consumer to progress engine: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_comch_consumer_task_post_recv_set_conf(obj, task_cb, error_cb, task_pool_size);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_comch_consumer_destroy(obj));
		throw std::runtime_error{"Failed to create doca_comch_consumer task pool: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_ctx_set_user_data(doca_comch_consumer_as_ctx(obj), callback_user_data);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_comch_consumer_destroy(obj));
		throw std::runtime_error{"Failed to set doca_comch_consumer user data: "s + doca_error_get_name(ret)};
	}

	ret = doca_ctx_start(doca_comch_consumer_as_ctx(obj));
	if (ret != DOCA_ERROR_IN_PROGRESS) {
		static_cast<void>(doca_comch_consumer_destroy(obj));
		throw std::runtime_error{"Failed to start doca_comch_consumer: "s + doca_error_get_name(ret)};
	}

	return obj;
}

doca_comch_producer *make_comch_producer(doca_comch_connection *conn,
					 doca_pe *pe,
					 uint32_t task_pool_size,
					 doca_data callback_user_data,
					 doca_comch_producer_task_send_completion_cb_t task_cb,
					 doca_comch_producer_task_send_completion_cb_t error_cb)
{
	doca_error_t ret;

	doca_comch_producer *obj;
	ret = doca_comch_producer_create(conn, &obj);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create doca_comch_producer: "s + doca_error_get_name(ret)};
	}
	DOCA_LOG_TRC(
		"Created producer using; conn: %p, pe: %p, task_pool_size: %u, callback_user_data: 0x%lX, task_cb: %p, error_cb: %p ",
		conn,
		pe,
		task_pool_size,
		callback_user_data.u64,
		task_cb,
		error_cb);

	ret = doca_pe_connect_ctx(pe, doca_comch_producer_as_ctx(obj));
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_comch_producer_destroy(obj));
		throw std::runtime_error{"Failed to connect doca_comch_producer to progress engine: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_comch_producer_task_send_set_conf(obj, task_cb, error_cb, task_pool_size);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_comch_producer_destroy(obj));
		throw std::runtime_error{"Failed to create doca_comch_producer task pool: "s +
					 doca_error_get_name(ret)};
	}

	ret = doca_ctx_set_user_data(doca_comch_producer_as_ctx(obj), callback_user_data);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_comch_producer_destroy(obj));
		throw std::runtime_error{"Failed to set doca_comch_producer user data: "s + doca_error_get_name(ret)};
	}

	ret = doca_ctx_start(doca_comch_producer_as_ctx(obj));
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_comch_producer_destroy(obj));
		throw std::runtime_error{"Failed to start doca_comch_producer: "s + doca_error_get_name(ret)};
	}

	return obj;
}

doca_rdma *make_rdma_context(doca_dev *dev, doca_pe *pe, doca_data ctx_user_data, uint32_t permissions)
{
	doca_error_t ret;

	doca_rdma *rdma;
	ret = doca_rdma_create(dev, &rdma);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create rdma instance: "s + doca_error_get_name(ret)};
	}

	ret = doca_rdma_set_permissions(rdma, permissions);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_rdma_destroy(rdma));
		throw std::runtime_error{"Failed to set doca_rdma permissions: "s + doca_error_get_name(ret)};
	}

	ret = doca_pe_connect_ctx(pe, doca_rdma_as_ctx(rdma));
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_rdma_destroy(rdma));
		throw std::runtime_error{"Failed to attach doca_rdma to doca_pe: "s + doca_error_get_name(ret)};
	}

	ret = doca_ctx_set_user_data(doca_rdma_as_ctx(rdma), ctx_user_data);
	if (ret != DOCA_SUCCESS) {
		static_cast<void>(doca_rdma_destroy(rdma));
		throw std::runtime_error{"Failed to set doca_rdma user data: "s + doca_error_get_name(ret)};
	}

	return rdma;
}

bool is_ctx_running(doca_ctx *ctx) noexcept
{
	doca_ctx_states cur_state = DOCA_CTX_STATE_IDLE;
	static_cast<void>(doca_ctx_get_state(ctx, &cur_state));
	return cur_state == DOCA_CTX_STATE_RUNNING;
}

doca_error_t stop_context(doca_ctx *ctx, doca_pe *pe) noexcept
{
	std::vector<doca_task *> ctx_tasks;
	return stop_context(ctx, pe, ctx_tasks);
}

doca_error_t stop_context(doca_ctx *ctx, doca_pe *pe, std::vector<doca_task *> &ctx_tasks) noexcept
{
	/* If all tasks are completed, they can be released immediately to avoid any complaints during stop */
	if (!ctx_tasks.empty()) {
		size_t num_inflight_tasks = 0;
		static_cast<void>(doca_ctx_get_num_inflight_tasks(ctx, &num_inflight_tasks));
		if (num_inflight_tasks == 0) {
			for (auto *task : ctx_tasks)
				doca_task_free(task);
			ctx_tasks.clear();
		}
	}

	auto ret = doca_ctx_stop(ctx);
	if (ret == DOCA_SUCCESS)
		return DOCA_SUCCESS;

	if (ret != DOCA_ERROR_AGAIN && ret != DOCA_ERROR_IN_USE && ret != DOCA_ERROR_IN_PROGRESS)
		return ret;

	/* Submitted tasks require the context to start stopping to flush them out (via the error callback) before they
	 * can be released, progress the context until all pending tasks have been completed.
	 */
	if (!ctx_tasks.empty()) {
		size_t num_inflight_tasks = 0;
		do {
			static_cast<void>(doca_ctx_get_num_inflight_tasks(ctx, &num_inflight_tasks));
			for (size_t ii = 0; ii != num_inflight_tasks; ++ii)
				static_cast<void>(doca_pe_progress(pe));
		} while (num_inflight_tasks != 0);

		for (auto *task : ctx_tasks)
			doca_task_free(task);
		ctx_tasks.clear();
	}

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

void register_cli_argument(doca_argp_type type,
			   char const *short_name,
			   char const *long_name,
			   char const *description,
			   value_requirement requirement,
			   value_multiplicity multiplicity,
			   doca_argp_param_cb_t callback)
{
	if (!short_name && !long_name) {
		throw std::runtime_error{"Unable to register arg parser parameter with no name"};
	}
	doca_error_t ret;
	doca_argp_param *param{nullptr};

	ret = doca_argp_param_create(&param);
	if (ret != DOCA_SUCCESS) {
		throw std::runtime_error{"Failed to create arg parser parameter"};
	}

	if (short_name != nullptr) {
		doca_argp_param_set_short_name(param, short_name);
	}
	if (long_name != nullptr) {
		doca_argp_param_set_long_name(param, long_name);
	}
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, callback);
	doca_argp_param_set_type(param, type);
	if (requirement.is_required) {
		doca_argp_param_set_mandatory(param);
	}
	if (multiplicity.support_multiple_values) {
		doca_argp_param_set_multiplicity(param);
	}

	ret = doca_argp_register_param(param);
	if (ret != DOCA_SUCCESS) {
		std::string message{"Unable to register arg parser parameter: "};
		if (short_name) {
			message += "-";
			message += *short_name;
			if (long_name) {
				message += " aka ";
			}
		}

		if (long_name) {
			message += "--";
			message += long_name;
		}

		message += " reason: ";
		message += doca_error_get_descr(ret);

		throw std::runtime_error{message};
	}
}

void create_doca_logger_backend(void) noexcept
{
	doca_error_t ret;

	doca_log_backend *stdout_logger = nullptr;

	/* Register a logger backend */
	ret = doca_log_backend_create_standard();
	if (ret != DOCA_SUCCESS) {
		printf("doca_log_backend_create_standard() failed: %s\n", doca_error_get_name(ret));
		fflush(stdout);
		fflush(stderr);
		exit(EXIT_FAILURE);
	}

	ret = doca_log_backend_create_with_file_sdk(stdout, &stdout_logger);
	if (ret != DOCA_SUCCESS) {
		printf("doca_log_backend_create_with_file_sdk() failed: %s\n", doca_error_get_name(ret));
		fflush(stdout);
		fflush(stderr);
		exit(EXIT_FAILURE);
	}

	ret = doca_log_backend_set_sdk_level(stdout_logger, DOCA_LOG_LEVEL_WARNING);
	if (ret != DOCA_SUCCESS) {
		printf("doca_log_backend_set_sdk_level() failed: %s\n", doca_error_get_name(ret));
		fflush(stdout);
		fflush(stderr);
		exit(EXIT_FAILURE);
	}
}

doca_ec_matrix_type matrix_type_from_string(std::string const &matrix_type)
{
	if (matrix_type == "vandermonde") {
		return DOCA_EC_MATRIX_TYPE_VANDERMONDE;
	}

	if (matrix_type == "cauchy") {
		return DOCA_EC_MATRIX_TYPE_CAUCHY;
	}

	throw storage::runtime_error{DOCA_ERROR_INVALID_VALUE, "Unknown doca_ec matrix type: "s + matrix_type};
}

} /* namespace storage */
