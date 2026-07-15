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

#include <server/thread.hpp>

#include <algorithm>
#include <cstring>
#include <system_error>

#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_log.h>

#include <remote_offload_common/doca_utils.hpp>
#include <remote_offload_common/runtime_error.hpp>
#include <remote_offload_common/os_utils.hpp>

DOCA_LOG_REGISTER(server::thread);

namespace remote_offload {
namespace server {

thread::~thread()
{
	auto const result = cleanup();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to cleanup successfully: %s", doca_error_get_name(result));
	}
}

thread::thread()
	: m_core_idx{0},
	  m_remote_consumer_id{0},
	  m_tcp_rx_byte_count{0},
	  m_io_memory_size{0},
	  m_max_concurrent_messages{0},
	  m_max_message_length{0},
	  m_free_tasks_count{0},
	  m_socket{},
	  m_io_memory{nullptr},
	  m_io_mmap{nullptr},
	  m_io_inventory{nullptr},
	  m_pe{nullptr},
	  m_consumer{nullptr},
	  m_producer{nullptr},
	  m_free_tasks_list{nullptr},
	  m_shared_thread_control{nullptr},
	  m_synchronized_data_lock{},
	  m_synchronized_data{{}, DOCA_ERROR_AGAIN},
	  m_thread{}
{
}

void thread::init(doca_dev *dev,
		  uint32_t core_idx,
		  uint32_t max_concurrent_messages,
		  uint32_t max_message_length,
		  remote_offload::server::comch_control_channel *control_channel,
		  remote_offload::thread_control *shared_thread_control)
{
	m_core_idx = core_idx;
	m_max_concurrent_messages = max_concurrent_messages;
	m_max_message_length = max_message_length + sizeof(control::message_header);
	m_shared_thread_control = shared_thread_control;
	/* init thread control, no need for a lock as the thread has not started yet */

	try {
		m_thread = std::thread{thread_proc_wrapper, this, dev, control_channel};
		/* Wait for initialization to complete. All initialization is done within the thread
		 * body to ensure memory allocations are taken from the memory closest to the CPU in NUMA systems
		 */
		for (;;) {
			std::this_thread::yield();

			m_synchronized_data_lock.lock();
			auto const status = m_synchronized_data.result;
			m_synchronized_data_lock.unlock();

			if (status == DOCA_SUCCESS) {
				break;
			}

			if (status != DOCA_ERROR_AGAIN) {
				throw remote_offload::runtime_error{status,
								    "Failed to initialise thread on core: " +
									    std::to_string(core_idx)};
			}
		}
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("Failed to create thread using core: %u. Error: %s", core_idx, ex.what());
		static_cast<void>(cleanup());
		throw;
	}
}

doca_error_t thread::launch(remote_offload::tcp_socket socket) noexcept
{
	doca_error_t status;
	try {
		m_synchronized_data_lock.lock();
		status = m_synchronized_data.result;
		if (status == DOCA_SUCCESS) {
			m_synchronized_data.new_socket = std::move(socket);
		}
		m_synchronized_data_lock.unlock();
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("Failed to lock mutex: %s", ex.what());
		status = DOCA_ERROR_OPERATING_SYSTEM;
		m_shared_thread_control->quit_flag = true;
		m_shared_thread_control->error_flag = true;
	}

	if (status != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Tried to launch a thread which was not ready to run: %s", doca_error_get_name(status));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

bool thread::is_running() noexcept
{
	doca_error_t status;
	bool has_pending_socket;
	try {
		m_synchronized_data_lock.lock();
		status = m_synchronized_data.result;
		has_pending_socket = m_synchronized_data.new_socket.is_valid();
		m_synchronized_data_lock.unlock();
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("Failed to lock mutex: %s", ex.what());
		status = DOCA_ERROR_OPERATING_SYSTEM;
		has_pending_socket = false;
		m_shared_thread_control->quit_flag = true;
		m_shared_thread_control->error_flag = true;
	}

	return has_pending_socket || status == DOCA_ERROR_AGAIN;
}

void thread::join() noexcept
{
	if (m_thread.joinable()) {
		try {
			m_thread.join();
		} catch (std::exception const &ex) {
			DOCA_LOG_ERR("Failed to join thread using core: %u. Error: %s", m_core_idx, ex.what());
		}
	}
}

doca_error_t thread::get_thread_result() noexcept
{
	doca_error_t status;
	try {
		m_synchronized_data_lock.lock();
		status = m_synchronized_data.result;
		m_synchronized_data_lock.unlock();
	} catch (std::exception const &ex) {
		DOCA_LOG_ERR("Failed to lock mutex: %s", ex.what());
		status = DOCA_ERROR_OPERATING_SYSTEM;
		m_shared_thread_control->quit_flag = true;
		m_shared_thread_control->error_flag = true;
	}

	return status;
}

void thread::doca_comch_producer_task_send_completion_cb(doca_comch_producer_task_send *task,
							 doca_data task_user_data,
							 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);
	static_cast<void>(ctx_user_data);

	auto *const self = static_cast<thread *>(ctx_user_data.ptr);
	self->m_free_tasks_list[self->m_free_tasks_count++] = task;
}

void thread::doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
						    doca_data task_user_data,
						    doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	auto *const self = static_cast<thread *>(ctx_user_data.ptr);
	self->m_shared_thread_control->quit_flag = true;
	self->m_shared_thread_control->error_flag = true;

	DOCA_LOG_ERR("doca_comch_producer_task_send: %p failed : %s",
		     task,
		     doca_error_get_name(doca_task_get_status(doca_comch_producer_task_send_as_task(task))));
}

void thread::doca_comch_consumer_task_post_recv_completion_cb(doca_comch_consumer_task_post_recv *task,
							      doca_data task_user_data,
							      doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	static_cast<thread *>(ctx_user_data.ptr)->process_response(task);
}

void thread::doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
							 doca_data task_user_data,
							 doca_data ctx_user_data) noexcept
{
	static_cast<void>(task_user_data);

	auto *const self = static_cast<thread *>(ctx_user_data.ptr);

	if (self->m_shared_thread_control->quit_flag == false) {
		DOCA_LOG_ERR(
			"doca_comch_consumer_task_post_recv: %p failed : %s",
			task,
			doca_error_get_name(doca_task_get_status(doca_comch_consumer_task_post_recv_as_task(task))));
	}

	self->m_shared_thread_control->quit_flag = true;
	self->m_shared_thread_control->error_flag = true;
}

void thread::thread_proc_wrapper(server::thread *self,
				 doca_dev *dev,
				 remote_offload::server::comch_control_channel *control_channel) noexcept
{
	doca_error_t result;
	try {
		result = self->thread_proc(dev, control_channel);
	} catch (remote_offload::runtime_error const &ex) {
		result = ex.get_doca_error();
		DOCA_LOG_ERR("Thread %p Failed. Error: %s : %s",
			     self,
			     doca_error_get_name(ex.get_doca_error()),
			     ex.what());
	} catch (std::system_error const &ex) {
		result = DOCA_ERROR_OPERATING_SYSTEM;
		DOCA_LOG_ERR("Thread %p Failed. system error: category: %s(%d), message: %s",
			     self,
			     ex.code().category().name(),
			     ex.code().value(),
			     ex.what());
	} catch (std::exception const &ex) {
		result = DOCA_ERROR_UNEXPECTED;
		DOCA_LOG_ERR("Thread %p Failed. Unexpected exception: %s", self, ex.what());
	}

	if (self->m_shared_thread_control->error_flag) {
		result = DOCA_ERROR_UNKNOWN;
	}

	try {
		self->m_synchronized_data_lock.lock();
		self->m_synchronized_data.result = result;
		self->m_synchronized_data_lock.unlock();
	} catch (std::system_error const &ex) {
		result = DOCA_ERROR_OPERATING_SYSTEM;
		DOCA_LOG_ERR("Thread %p Failed. system error: category: %s(%d), message: %s",
			     self,
			     ex.code().category().name(),
			     ex.code().value(),
			     ex.what());
	}

	if (result == DOCA_SUCCESS) {
		DOCA_LOG_INFO("Thread %p completed successfully", self);
	} else {
		DOCA_LOG_INFO("Thread %p completed with error: %s", self, doca_error_get_name(result));
		self->m_shared_thread_control->quit_flag = true;
		self->m_shared_thread_control->error_flag = true;
	}
}

doca_error_t thread::thread_proc(doca_dev *dev, remote_offload::server::comch_control_channel *control_channel)
{
	doca_error_t result;

	/*
	 * Init phase
	 */
	create_local_objects(dev, control_channel->get_connection());
	result = exchange_consumer_ids(control_channel);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to exchange consumer ID values with host: %s", doca_error_get_name(result));
		return result;
	}

	result = submit_initial_tasks();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit initial tasks: %s", doca_error_get_name(result));
		return result;
	}

	/* notify application the thread is ready */
	m_synchronized_data_lock.lock();
	m_synchronized_data.result = result;
	m_synchronized_data_lock.unlock();

	/*
	 * Data path phase
	 */
	for (;;) {
		if (m_shared_thread_control->quit_flag)
			return DOCA_ERROR_SHUTDOWN;

		m_synchronized_data_lock.lock();
		if (m_synchronized_data.new_socket.is_valid()) {
			m_socket = std::move(m_synchronized_data.new_socket);
			m_synchronized_data.result = DOCA_ERROR_AGAIN;
		}
		m_synchronized_data_lock.unlock();

		if (!m_socket.is_valid()) {
			std::this_thread::yield();
			continue;
		}

		result = tcp_thread_proc();
		m_socket.close();
		if (result == DOCA_ERROR_CONNECTION_RESET) {
			m_synchronized_data_lock.lock();
			m_synchronized_data.result = DOCA_SUCCESS;
			m_synchronized_data_lock.unlock();
		} else {
			return result;
		}
	}
}

doca_error_t thread::tcp_thread_proc() noexcept
{
	m_tcp_rx_buffer.resize(m_max_message_length);
	m_tcp_rx_byte_count = 0;

	for (;;) {
		if (m_shared_thread_control->quit_flag)
			return DOCA_SUCCESS;

		/* progress the engine until all pending task completions are processed */
		while (doca_pe_progress(m_pe))
			;

		/* Skip reading from TCP until producer tasks become free again to send the request to the host */
		if (m_free_tasks_count == 0)
			continue;

		m_socket.poll();
		if (!m_socket.can_read())
			continue;

		/*
		 * Currently the data path acts as there is only one message in flight at a time per thread.
		 */
		auto const read_count = m_socket.read(m_tcp_rx_buffer.data() + m_tcp_rx_byte_count,
						      m_max_message_length - m_tcp_rx_byte_count);
		if (read_count == 0) {
			DOCA_LOG_INFO("Client disconnected");
			return DOCA_ERROR_CONNECTION_RESET;
		}

		if (read_count < 0) {
			DOCA_LOG_ERR("Read from socket failed");
			return DOCA_ERROR_IO_FAILED;
		}

		m_tcp_rx_byte_count += read_count;

check_next:
		if (m_tcp_rx_byte_count < sizeof(control::message_header)) {
			std::this_thread::yield();
			continue;
		}

		control::message_header hdr{};
		control::decode(reinterpret_cast<uint8_t *>(m_tcp_rx_buffer.data()), hdr);
		if (hdr.wire_size > m_max_message_length) {
			DOCA_LOG_ERR(
				"Received indication of a TCP message that is too large. Requested %u bytes, max is %u",
				hdr.wire_size,
				m_max_message_length);
			m_shared_thread_control->quit_flag = true;
			m_shared_thread_control->error_flag = true;
			return DOCA_ERROR_INVALID_VALUE;
		}

		if (hdr.wire_size <= m_tcp_rx_byte_count) {
			auto const result = send_request(m_tcp_rx_buffer.data(), hdr.wire_size);
			if (result != DOCA_SUCCESS)
				return result;

			m_tcp_rx_byte_count -= hdr.wire_size;
			if (m_tcp_rx_byte_count != 0) {
				::memmove(m_tcp_rx_buffer.data(),
					  m_tcp_rx_buffer.data() + hdr.wire_size,
					  m_tcp_rx_byte_count);
				goto check_next;
			}
		}
	}
}

doca_error_t thread::cleanup() noexcept
{
	doca_error_t result;

	join();

	std::vector<doca_buf *> bufs;
	bufs.reserve(m_comch_recv_tasks.size() + m_comch_send_tasks.size());
	std::transform(std::begin(m_comch_recv_tasks),
		       std::end(m_comch_recv_tasks),
		       std::back_inserter(bufs),
		       doca_comch_consumer_task_post_recv_get_buf);
	std::transform(std::begin(m_comch_send_tasks),
		       std::end(m_comch_send_tasks),
		       std::back_inserter(bufs),
		       [](auto *task) {
			       return const_cast<doca_buf *>(doca_comch_producer_task_send_get_buf(task));
		       });

	if (m_consumer != nullptr) {
		std::vector<doca_task *> tasks;
		tasks.reserve(m_comch_recv_tasks.size());
		std::transform(std::begin(m_comch_recv_tasks),
			       std::end(m_comch_recv_tasks),
			       std::back_inserter(tasks),
			       doca_comch_consumer_task_post_recv_as_task);
		result = remote_offload::stop_context(doca_comch_consumer_as_ctx(m_consumer), m_pe, tasks);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_comch_consumer");
			return result;
		}

		result = doca_comch_consumer_destroy(m_consumer);
		if (result == DOCA_SUCCESS) {
			m_consumer = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy doca_comch_consumer");
			return result;
		}
	}

	if (m_producer != nullptr) {
		std::vector<doca_task *> tasks;
		tasks.reserve(m_comch_send_tasks.size());
		std::transform(std::begin(m_comch_send_tasks),
			       std::end(m_comch_send_tasks),
			       std::back_inserter(tasks),
			       doca_comch_producer_task_send_as_task);
		result = remote_offload::stop_context(doca_comch_producer_as_ctx(m_producer), m_pe, tasks);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_comch_producer");
			return result;
		}

		result = doca_comch_producer_destroy(m_producer);
		if (result == DOCA_SUCCESS) {
			m_producer = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy doca_comch_producer");
			return result;
		}
	}

	if (m_pe != nullptr) {
		result = doca_pe_destroy(m_pe);
		if (result == DOCA_SUCCESS) {
			m_pe = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy doca_pe");
			return result;
		}
	}

	for (auto *buf : bufs) {
		doca_buf_dec_refcount(buf, nullptr);
	}

	if (m_io_inventory != nullptr) {
		result = doca_buf_inventory_stop(m_io_inventory);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_buf_inventory");
			return result;
		}

		result = doca_buf_inventory_destroy(m_io_inventory);
		if (result == DOCA_SUCCESS) {
			m_io_inventory = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy doca_buf_inventory");
			return result;
		}
	}

	if (m_io_mmap != nullptr) {
		result = doca_mmap_stop(m_io_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop doca_mmap");
			return result;
		}

		result = doca_mmap_destroy(m_io_mmap);
		if (result == DOCA_SUCCESS) {
			m_io_mmap = nullptr;
		} else {
			DOCA_LOG_ERR("Failed to destroy doca_mmap");
			return result;
		}
	}

	if (m_io_memory != nullptr) {
		remote_offload::aligned_free(m_io_memory);
		m_io_memory = nullptr;
	}

	return DOCA_SUCCESS;
}

void thread::create_local_objects(doca_dev *dev, doca_comch_connection *comch_connection)
{
	doca_error_t result;

	/*
	 * Create memory to store TCP requests and responses
	 */
	m_io_memory_size = m_max_concurrent_messages * m_max_message_length * 2;
	m_io_memory = static_cast<uint8_t *>(
		remote_offload::aligned_alloc(remote_offload::get_system_page_size(), m_io_memory_size));
	/* NOTE: double the memory is allocated as the comch receive tasks are independent and need a separate memory to
	 * put their result into
	 */
	if (m_io_memory == nullptr) {
		throw remote_offload::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate TCP receive buffers"};
	}

	m_free_tasks_list = static_cast<doca_comch_producer_task_send **>(
		remote_offload::aligned_alloc(64, m_max_concurrent_messages * sizeof(void *)));
	if (m_free_tasks_list == nullptr) {
		throw remote_offload::runtime_error{DOCA_ERROR_NO_MEMORY, "Failed to allocate free tasks list"};
	}

	m_free_tasks_count = 0;
	m_comch_send_tasks.reserve(m_max_concurrent_messages);
	m_comch_recv_tasks.reserve(m_max_concurrent_messages);

	/*
	 * Register memory for use with doca_task
	 */
	result = doca_mmap_create(&m_io_mmap);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_mmap"};
	}

	result = doca_mmap_add_dev(m_io_mmap, dev);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to add doca_dev to doca_mmap"};
	}

	result = doca_mmap_set_memrange(m_io_mmap, m_io_memory, m_io_memory_size);
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
	 * Create inventory
	 */
	result = doca_buf_inventory_create(m_max_concurrent_messages * 2, &m_io_inventory);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_buf_inventory"};
	}

	result = doca_buf_inventory_start(m_io_inventory);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to start doca_buf_inventory"};
	}

	/*
	 * Create progress engine
	 */
	result = doca_pe_create(&m_pe);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_pe"};
	}

	/*
	 * Create consumer
	 */
	result = doca_comch_consumer_create(comch_connection, m_io_mmap, &m_consumer);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_comch_consumer"};
	}

	result = doca_pe_connect_ctx(m_pe, doca_comch_consumer_as_ctx(m_consumer));
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to connect doca_comch_consumer to progress engine"};
	}

	result = doca_comch_consumer_task_post_recv_set_conf(m_consumer,
							     doca_comch_consumer_task_post_recv_completion_cb,
							     doca_comch_consumer_task_post_recv_error_cb,
							     m_max_concurrent_messages);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_comch_consumer task pool"};
	}

	result = doca_ctx_set_user_data(doca_comch_consumer_as_ctx(m_consumer), doca_data{.ptr = this});
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_comch_consumer user data"};
	}

	result = doca_ctx_start(doca_comch_consumer_as_ctx(m_consumer));
	if (result != DOCA_ERROR_IN_PROGRESS) {
		throw remote_offload::runtime_error{result, "Failed to start doca_comch_consumer"};
	}

	/*
	 * Create producer
	 */
	result = doca_comch_producer_create(comch_connection, &m_producer);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_comch_producer"};
	}

	result = doca_pe_connect_ctx(m_pe, doca_comch_producer_as_ctx(m_producer));
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to connect doca_comch_producer to progress engine"};
	}

	result = doca_comch_producer_task_send_set_conf(m_producer,
							doca_comch_producer_task_send_completion_cb,
							doca_comch_producer_task_send_error_cb,
							m_max_message_length);
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to create doca_comch_producer task pool"};
	}

	result = doca_ctx_set_user_data(doca_comch_producer_as_ctx(m_producer), doca_data{.ptr = this});
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to set doca_comch_producer user data"};
	}

	result = doca_ctx_start(doca_comch_producer_as_ctx(m_producer));
	if (result != DOCA_SUCCESS) {
		throw remote_offload::runtime_error{result, "Failed to start doca_comch_producer"};
	}
}

doca_error_t thread::exchange_consumer_ids(remote_offload::server::comch_control_channel *control_channel)
{
	doca_error_t result;
	uint32_t local_consumer_id;
	static_cast<void>(doca_comch_consumer_get_id(m_consumer, &local_consumer_id));

	control::exchange_consumer_id_data payload{local_consumer_id};
	control::message_header hdr{sizeof(control::message_header) + sizeof(control::exchange_consumer_id_data),
				    control::message_id::exchange_consumer_id_request};
	std::array<uint8_t, sizeof(control::message_header) + sizeof(control::exchange_consumer_id_data)> message_buf;
	control::encode(control::encode(message_buf.data(), hdr), payload);
	result = control_channel->send_control_message(message_buf.data(), message_buf.size());
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send exchange consumer id request");
		return result;
	}

	for (;;) {
		if (m_shared_thread_control->quit_flag) {
			m_shared_thread_control->error_flag = true;
			DOCA_LOG_WARN("Aborted while exchanging consumer IDs with host");
			return DOCA_ERROR_BAD_STATE;
		}

		control_channel->poll_pe();
		auto message = control_channel->get_pending_control_message();
		if (message.empty()) {
			std::this_thread::yield();
			continue;
		}

		control::decode(message.data(), hdr);

		if (hdr.msg_id != control::message_id::exchange_consumer_id_response) {
			DOCA_LOG_WARN("Unexpected message: %u while waiting for exchange consumer id response",
				      static_cast<uint32_t>(hdr.msg_id));
			continue;
		}

		if (hdr.wire_size != sizeof(control::message_header) + sizeof(control::exchange_consumer_id_data)) {
			DOCA_LOG_WARN("Received malformed exchange consumer id response");
			return DOCA_ERROR_IO_FAILED;
		}

		control::decode(message.data() + sizeof(control::message_header), payload);
		m_remote_consumer_id = payload.consumer_id;

		DOCA_LOG_INFO("Thread running on core: %u completed consumer ID exchange. Local: %u, remote: %u",
			      m_core_idx,
			      local_consumer_id,
			      m_remote_consumer_id);
		break;
	}

	/* poll consumer until it is ready */
	for (;;) {
		static_cast<void>(doca_pe_progress(m_pe));

		doca_ctx_states ctx_state;
		result = doca_ctx_get_state(doca_comch_consumer_as_ctx(m_consumer), &ctx_state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query comch consumer state: %s", doca_error_get_name(result));
			return result;
		}

		if (ctx_state == DOCA_CTX_STATE_RUNNING) {
			break;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t thread::submit_initial_tasks() noexcept
{
	doca_error_t result;
	uint8_t *data_addr = m_io_memory;
	for (uint32_t ii = 0; ii != m_max_concurrent_messages; ++ii) {
		doca_buf *buf;
		doca_comch_producer_task_send *send_task;
		result = doca_buf_inventory_buf_get_by_addr(m_io_inventory,
							    m_io_mmap,
							    data_addr,
							    m_max_message_length,
							    &buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate doca_buf: %s", doca_error_get_name(result));
			return result;
		}

		data_addr += m_max_message_length;

		result = doca_comch_producer_task_send_alloc_init(m_producer,
								  buf,
								  nullptr,
								  0,
								  m_remote_consumer_id,
								  &send_task);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate doca_comch_producer_task_send to send request to host");
			static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
			return result;
		}

		m_free_tasks_list[m_free_tasks_count++] = send_task;
		m_comch_send_tasks.push_back(send_task);
	}

	for (uint32_t ii = 0; ii != m_max_concurrent_messages; ++ii) {
		doca_buf *buf;
		doca_comch_consumer_task_post_recv *recv_task;
		result = doca_buf_inventory_buf_get_by_addr(m_io_inventory,
							    m_io_mmap,
							    data_addr,
							    m_max_message_length,
							    &buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate doca_buf: %s", doca_error_get_name(result));
			return result;
		}

		data_addr += m_max_message_length;

		result = doca_comch_consumer_task_post_recv_alloc_init(m_consumer, buf, &recv_task);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to allocate doca_comch_producer_task_send to send request to host");
			static_cast<void>(doca_buf_dec_refcount(buf, nullptr));
			return result;
		}

		m_comch_recv_tasks.push_back(recv_task);
	}

	for (auto *task : m_comch_recv_tasks) {
		result = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(task));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to submit initial comch consumer task: %s", doca_error_get_name(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

doca_error_t thread::send_request(void *message, size_t message_len) noexcept
{
	auto *task = m_free_tasks_list[--m_free_tasks_count];

	auto *buf = const_cast<doca_buf *>(doca_comch_producer_task_send_get_buf(task));
	void *data = nullptr;

	static_cast<void>(doca_buf_get_data(buf, &data));

	::memcpy(data, message, message_len);
	static_cast<void>(doca_buf_set_data_len(buf, message_len));

	doca_error_t result;
	do {
		result = doca_task_submit(doca_comch_producer_task_send_as_task(task));
	} while (result == DOCA_ERROR_AGAIN);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to submit doca_comch_producer_task_send: %s", doca_error_get_name(result));
	}

	return result;
}

void thread::process_response(doca_comch_consumer_task_post_recv *task) noexcept
{
	auto *buf = doca_comch_consumer_task_post_recv_get_buf(task);
	void *data = nullptr;
	size_t data_len = 0;

	static_cast<void>(doca_buf_get_data(buf, &data));
	static_cast<void>(doca_buf_get_data_len(buf, &data_len));

	m_socket.poll();
	if (!m_socket.can_write()) {
		DOCA_LOG_ERR("Client unexpectedly closed socket");
		m_shared_thread_control->error_flag = true;
		m_shared_thread_control->quit_flag = true;
		return;
	}

	auto written_count = m_socket.write(static_cast<uint8_t const *>(data), data_len);
	if (written_count < 0 || static_cast<size_t>(written_count) != data_len) {
		DOCA_LOG_ERR("Failed to write response to TCP client.");
		m_shared_thread_control->error_flag = true;
		m_shared_thread_control->quit_flag = true;
		return;
	}

	static_cast<void>(doca_buf_reset_data_len(buf));
	auto const result = doca_task_submit(doca_comch_consumer_task_post_recv_as_task(task));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to re-submit doca_comch_consumer_task_post_recv: %s", doca_error_get_name(result));
		m_shared_thread_control->error_flag = true;
		m_shared_thread_control->quit_flag = true;
	}
}

} /* namespace server */
} /* namespace remote_offload */