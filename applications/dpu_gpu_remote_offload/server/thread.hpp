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

#include <mutex>
#include <thread>
#include <vector>

#include <doca_buf_inventory.h>
#include <doca_comch.h>
#include <doca_comch_consumer.h>
#include <doca_comch_producer.h>
#include <doca_dev.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <remote_offload_common/tcp_socket.hpp>
#include <remote_offload_common/thread_control.hpp>

#include <server/comch_control_channel.hpp>

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_SERVER_THREAD_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_SERVER_THREAD_HPP_

namespace remote_offload {
namespace server {

class thread {
public:
	~thread();
	thread();
	thread(thread const &) = delete;
	thread(thread &&) noexcept = delete;
	thread &operator=(thread const &) = delete;
	thread &operator=(thread &&) noexcept = delete;

	void init(doca_dev *dev,
		  uint32_t core_idx,
		  uint32_t max_concurrent_messages,
		  uint32_t max_message_length,
		  remote_offload::server::comch_control_channel *control_channel,
		  remote_offload::thread_control *shared_thread_control);

	doca_error_t launch(remote_offload::tcp_socket socket) noexcept;

	bool is_running() noexcept;

	void join() noexcept;

	doca_error_t get_thread_result() noexcept;

private:
	/* Data that should only ever be accessed while holding the lock. Allows the thread and its owner to safely
	 * exchange data
	 */
	struct synchronized_data {
		remote_offload::tcp_socket new_socket;
		doca_error_t result;
	};

	static void doca_comch_producer_task_send_completion_cb(doca_comch_producer_task_send *task,
								doca_data task_user_data,
								doca_data ctx_user_data) noexcept;

	static void doca_comch_producer_task_send_error_cb(doca_comch_producer_task_send *task,
							   doca_data task_user_data,
							   doca_data ctx_user_data) noexcept;

	static void doca_comch_consumer_task_post_recv_completion_cb(doca_comch_consumer_task_post_recv *task,
								     doca_data task_user_data,
								     doca_data ctx_user_data) noexcept;

	static void doca_comch_consumer_task_post_recv_error_cb(doca_comch_consumer_task_post_recv *task,
								doca_data task_user_data,
								doca_data ctx_user_data) noexcept;

	static void thread_proc_wrapper(server::thread *self,
					doca_dev *dev,
					remote_offload::server::comch_control_channel *control_channel) noexcept;
	doca_error_t thread_proc(doca_dev *dev, remote_offload::server::comch_control_channel *control_channel);

	doca_error_t cleanup() noexcept;
	void create_local_objects(doca_dev *dev, doca_comch_connection *comch_connection);
	doca_error_t exchange_consumer_ids(remote_offload::server::comch_control_channel *control_channel);
	doca_error_t submit_initial_tasks() noexcept;

	doca_error_t tcp_thread_proc() noexcept;
	doca_error_t send_request(void *message, size_t message_len) noexcept;
	void process_response(doca_comch_consumer_task_post_recv *task) noexcept;

	uint32_t m_core_idx;
	uint32_t m_remote_consumer_id;
	uint32_t m_tcp_rx_byte_count;
	uint32_t m_io_memory_size;
	uint32_t m_max_concurrent_messages;
	uint32_t m_max_message_length;
	uint32_t m_free_tasks_count;
	remote_offload::tcp_socket m_socket;
	uint8_t *m_io_memory;
	doca_mmap *m_io_mmap;
	doca_buf_inventory *m_io_inventory;
	doca_pe *m_pe;
	doca_comch_consumer *m_consumer;
	doca_comch_producer *m_producer;
	doca_comch_producer_task_send **m_free_tasks_list;
	remote_offload::thread_control *m_shared_thread_control;
	std::mutex m_synchronized_data_lock;
	synchronized_data m_synchronized_data;
	std::vector<doca_comch_consumer_task_post_recv *> m_comch_recv_tasks;
	std::vector<doca_comch_producer_task_send *> m_comch_send_tasks;
	std::vector<uint8_t> m_tcp_rx_buffer;
	std::thread m_thread;
};

} /* namespace server */
} /* namespace remote_offload */

#endif // APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_SERVER_THREAD_HPP_