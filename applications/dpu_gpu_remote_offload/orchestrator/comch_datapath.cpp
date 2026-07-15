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

#include <orchestrator/comch_datapath.hpp>

#include <math.h>

#include <doca_error.h>
#include <doca_gpunetio.h>
#include <doca_log.h>
#include <doca_mmap.h>

#include <remote_offload_common/control_message.hpp>
#include <remote_offload_common/runtime_error.hpp>

DOCA_LOG_REGISTER(orchestrator::comch_datapath);

namespace {
void doca_comch_producer_task_send_completion_cb(doca_comch_producer_task_send *task,
						 doca_data task_user_data,
						 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);
	static_cast<void>(task_user_data);
}

void doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
					    doca_data task_user_data,
					    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);
	static_cast<void>(task_user_data);
}

void doca_comch_consumer_task_post_recv_completion_cb(doca_comch_consumer_task_post_recv *task,
						      doca_data task_user_data,
						      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);
	static_cast<void>(task_user_data);
}

void doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
						 doca_data task_user_data,
						 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task);
	static_cast<void>(ctx_user_data);
	static_cast<void>(task_user_data);
}

} // namespace

namespace remote_offload {
namespace orchestrator {

comch_datapath::~comch_datapath()
{
	cleanup();
}

comch_datapath::comch_datapath(doca_dev *dev, doca_gpu *gpu, uint32_t num_gpu_consumers)
	: m_pe{nullptr},
	  m_dev{dev},
	  m_gpu{gpu},
	  m_next_local_consumer_id{1},
	  m_num_gpu_consumers{num_gpu_consumers},
	  m_gpu_thread_data{nullptr}
{
	doca_error_t result;
	try {
		result = doca_pe_create(&m_pe);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to create progress engine"};
		}
	} catch (...) {
		cleanup();
		throw;
	}
}

uint32_t comch_datapath::register_remote_consumer(uint32_t remote_consumer_id,
						  uint32_t max_concurrent_messages,
						  uint32_t max_message_length,
						  doca_comch_connection *connection)
{
	m_thread_data.emplace_back(new thread_data(m_dev,
						   m_gpu,
						   m_pe,
						   max_concurrent_messages,
						   max_message_length + sizeof(control::message_header),
						   remote_consumer_id,
						   m_next_local_consumer_id,
						   connection));

	return m_next_local_consumer_id++;
}

void comch_datapath::poll_pe() noexcept
{
	if (m_pe != nullptr) {
		static_cast<void>(doca_pe_progress(m_pe));
	}
}

bool comch_datapath::are_all_contexts_running() noexcept
{
	if (m_num_gpu_consumers != m_thread_data.size()) {
		return false;
	}

	doca_ctx_states state;
	for (auto *data : m_thread_data) {
		doca_ctx_get_state(doca_comch_consumer_as_ctx(data->m_consumer), &state);
		if (state != DOCA_CTX_STATE_RUNNING) {
			return false;
		}

		doca_ctx_get_state(doca_comch_producer_as_ctx(data->m_producer), &state);
		if (state != DOCA_CTX_STATE_RUNNING) {
			return false;
		}
	}
	return true;
}

gpu_thread_data *comch_datapath::get_gpu_thread_data()
{
	if (m_num_gpu_consumers != m_thread_data.size()) {
		throw remote_offload::runtime_error{
			DOCA_ERROR_INVALID_VALUE,
			"Failed to get gpu thread data, not all producers/consumers have been created"};
	}

	size_t buf_size = m_num_gpu_consumers * sizeof(gpu_thread_data);
	gpu_thread_data *thread_data_cpu;

	doca_error_t result = doca_gpu_mem_alloc(m_gpu,
						 buf_size,
						 0,
						 DOCA_GPU_MEM_TYPE_GPU_CPU,
						 (void **)&m_gpu_thread_data,
						 (void **)&thread_data_cpu);
	if (result != DOCA_SUCCESS || m_gpu_thread_data == nullptr || thread_data_cpu == nullptr) {
		throw remote_offload::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate GPU Data buffer"};
	}

	for (size_t i = 0; i < m_thread_data.size(); i++) {
		result = doca_comch_consumer_get_gpu_handle(m_thread_data[i]->m_consumer,
							    &m_thread_data[i]->m_gpu_consumer);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to get consumer GPU handle"};
		}

		result = doca_comch_producer_get_gpu_handle(m_thread_data[i]->m_producer,
							    &m_thread_data[i]->m_gpu_producer);
		if (result != DOCA_SUCCESS) {
			throw remote_offload::runtime_error{result, "Failed to get prodicer GPU handle"};
		}

		thread_data_cpu[i].consumer = m_thread_data[i]->m_gpu_consumer;
		thread_data_cpu[i].producer = m_thread_data[i]->m_gpu_producer;
		thread_data_cpu[i].buf_arr = m_thread_data[i]->m_gpu_io_buf_arr;
		thread_data_cpu[i].remote_consumer_id = m_thread_data[i]->m_remote_consumer_id;
		thread_data_cpu[i].local_consumer_id = m_thread_data[i]->m_local_consumer_id;
		thread_data_cpu[i].inflight_messages = m_thread_data[i]->m_inflight_messages;
		thread_data_cpu[i].inflight_messages_mask = m_thread_data[i]->m_inflight_messages_mask;
	}
	return m_gpu_thread_data;
}

void comch_datapath::cleanup() noexcept
{
	doca_error_t result;

	/* Stop all producers and consumers */
	for (auto *data : m_thread_data) {
		result = doca_ctx_stop(doca_comch_consumer_as_ctx(data->m_consumer));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop consumer: %s", doca_error_get_name(result));
		}

		result = doca_ctx_stop(doca_comch_producer_as_ctx(data->m_producer));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop producer: %s", doca_error_get_name(result));
		}
	}

	/* Wait for all the contexts to actually stop */
	while (!all_producers_consumers_idle()) {
		poll_pe();
	}

	for (auto *data : m_thread_data) {
		delete data;
	}

	m_thread_data.clear();

	if (m_pe != nullptr) {
		result = doca_pe_destroy(m_pe);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy progress engine: %s", doca_error_get_name(result));
		}
		m_pe = nullptr;
	}

	if (m_gpu_thread_data != nullptr) {
		result = doca_gpu_mem_free(m_gpu, m_gpu_thread_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to free GPU memory: %s", doca_error_get_name(result));
		}
		m_gpu_thread_data = nullptr;
	}
}

bool comch_datapath::all_producers_consumers_idle() noexcept
{
	doca_ctx_states state;
	for (auto *data : m_thread_data) {
		doca_ctx_get_state(doca_comch_consumer_as_ctx(data->m_consumer), &state);
		if (state != DOCA_CTX_STATE_IDLE) {
			return false;
		}

		doca_ctx_get_state(doca_comch_producer_as_ctx(data->m_producer), &state);
		if (state != DOCA_CTX_STATE_IDLE) {
			return false;
		}
	}
	return true;
}

comch_datapath::thread_data::~thread_data()
{
	doca_error_t result;
	if (m_consumer != nullptr) {
		result = doca_comch_consumer_destroy(m_consumer);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy consumer: %s", doca_error_get_name(result));
		}
	}

	if (m_producer != nullptr) {
		result = doca_comch_producer_destroy(m_producer);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy producer: %s", doca_error_get_name(result));
		}
	}

	if (m_io_memory != nullptr) {
		result = doca_gpu_mem_free(m_gpu, m_io_memory);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to free GPU Memory: %s", doca_error_get_name(result));
		}
	}

	if (m_inflight_messages) {
		result = doca_gpu_mem_free(m_gpu, m_inflight_messages);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to free GPU Memory: %s", doca_error_get_name(result));
		}
	}

	if (m_io_mmap != nullptr) {
		result = doca_mmap_stop(m_io_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop doca_mmap: %s", doca_error_get_name(result));
		}

		result = doca_mmap_destroy(m_io_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy doca_mmap: %s", doca_error_get_name(result));
		}
	}

	if (m_io_buf_arr != nullptr) {
		result = doca_buf_arr_stop(m_io_buf_arr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to stop doca_buf_arr: %s", doca_error_get_name(result));
		}

		result = doca_buf_arr_destroy(m_io_buf_arr);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_WARN("Failed to destroy doca_buf_arr: %s", doca_error_get_name(result));
		}
	}
}

comch_datapath::thread_data::thread_data(doca_dev *dev,
					 doca_gpu *gpu,
					 doca_pe *pe,
					 uint32_t max_concurrent_messages,
					 uint32_t max_message_length,
					 uint32_t remote_consumer_id,
					 uint32_t local_consumer_id,
					 doca_comch_connection *connection)
	: m_consumer{nullptr},
	  m_gpu_consumer{nullptr},
	  m_producer{nullptr},
	  m_gpu_producer{nullptr},
	  m_io_memory{nullptr},
	  m_io_mmap{nullptr},
	  m_io_buf_arr{nullptr},
	  m_gpu_io_buf_arr{nullptr},
	  m_remote_consumer_id{remote_consumer_id},
	  m_local_consumer_id{local_consumer_id},
	  m_gpu{gpu}
{
	doca_error_t result;

	// The number of elements in the inflight messages and buf array must bea power of 2, so find the next power of
	// 2 and create that many elements.
	auto next_exponent = ceil(log2(max_concurrent_messages));
	auto num_buf_elements = static_cast<uint32_t>(pow(2, next_exponent));
	/*
	 * Create memory to store TCP requests and responses
	 */
	uint32_t io_memory_size = num_buf_elements * max_message_length;
	result = doca_gpu_mem_alloc(gpu, io_memory_size, 0, DOCA_GPU_MEM_TYPE_GPU, (void **)&m_io_memory, nullptr);
	if (result != DOCA_SUCCESS || m_io_memory == nullptr) {
		throw remote_offload::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate TCP receive buffers"};
	}

	/*
	 * Register memory for use with comch
	 */

	result = doca_mmap_create(&m_io_mmap);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_mmap"};
	}

	result = doca_mmap_add_dev(m_io_mmap, dev);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to add doca_dev to doca_mmap"};
	}

	result = doca_mmap_set_memrange(m_io_mmap, m_io_memory, io_memory_size);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_mmap memory range"};
	}

	result = doca_mmap_set_permissions(m_io_mmap,
					   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_mmap access permissions"};
	}

	result = doca_mmap_start(m_io_mmap);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to start doca_mmap"};
	}

	/*
	 * Create Buf_arr and get GPU handle
	 */
	result = doca_buf_arr_create(num_buf_elements, &m_io_buf_arr);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_buf_arr"};
	}

	result = doca_buf_arr_set_target_gpu(m_io_buf_arr, gpu);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_buf_arr target GPU"};
	}

	result = doca_buf_arr_set_params(m_io_buf_arr, m_io_mmap, max_message_length, 0);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_buf_arr params"};
	}

	result = doca_buf_arr_start(m_io_buf_arr);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to start doca_buf_arr"};
	}

	result = doca_buf_arr_get_gpu_handle(m_io_buf_arr, &m_gpu_io_buf_arr);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to get doca_buf_arr gpu handle"};
	}

	/*
	 * Create consumer
	 */
	result = doca_comch_consumer_create(connection, m_io_mmap, &m_consumer);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_comch_consumer"};
	}

	result = doca_ctx_set_datapath_on_gpu(doca_comch_consumer_as_ctx(m_consumer), gpu);
	result = doca_pe_connect_ctx(pe, doca_comch_consumer_as_ctx(m_consumer));
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to connect doca_comch_consumer to progress engine"};
	}

	result = doca_comch_consumer_task_post_recv_set_conf(m_consumer,
							     doca_comch_consumer_task_post_recv_completion_cb,
							     doca_comch_consumer_task_post_recv_error_cb,
							     max_concurrent_messages);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_comch_consumer conf"};
	}

	result = doca_ctx_start(doca_comch_consumer_as_ctx(m_consumer));
	if (result != DOCA_ERROR_IN_PROGRESS) {
		throw remote_offload::runtime_error{result, "Failed to start doca_comch_consumer"};
	}

	/*
	 * Create producer
	 */
	result = doca_comch_producer_create(connection, &m_producer);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_comch_producer"};
	}

	result = doca_ctx_set_datapath_on_gpu(doca_comch_producer_as_ctx(m_producer), gpu);
	result = doca_pe_connect_ctx(pe, doca_comch_producer_as_ctx(m_producer));
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to connect doca_comch_producer to progress engine"};
	}

	result = doca_comch_producer_task_send_set_conf(m_producer,
							doca_comch_producer_task_send_completion_cb,
							doca_comch_producer_task_send_error_cb,
							max_concurrent_messages);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_comch_producer conf"};
	}

	result = doca_ctx_start(doca_comch_producer_as_ctx(m_producer));
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to start doca_comch_producer"};
	}

	// Allocate array for inflight messages
	result = doca_gpu_mem_alloc(gpu,
				    num_buf_elements * sizeof(inflight_msg_data),
				    0,
				    DOCA_GPU_MEM_TYPE_GPU,
				    (void **)&m_inflight_messages,
				    nullptr);
	if (result != DOCA_SUCCESS || m_inflight_messages == nullptr) {
		throw remote_offload::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate inflight message buffer"};
	}
	m_inflight_messages_mask = num_buf_elements - 1;
}

} // namespace orchestrator
} // namespace remote_offload