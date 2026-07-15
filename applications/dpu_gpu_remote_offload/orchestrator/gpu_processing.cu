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
#include <stdio.h>

#include <cuda_runtime.h>
#include <doca_gpunetio_dev_buf.cuh>
#include <doca_gpunetio_dev_comch.cuh>

#include <remote_offload_common/control_message.hpp>
#include <orchestrator/gpu_processing.h>

__device__ bool verify_message(uintptr_t addr, uint32_t msg_len);
__device__ uint32_t byte_swap_32(uint32_t data);
__device__ void reverse_buffer(uint8_t *head, uint32_t len);
__device__ void process_message(uintptr_t addr, uint32_t msg_len);

__device__ uint32_t byte_swap_32(uint32_t data)
{
	uint8_t *buf = (uint8_t *)&data;
	return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

__device__ bool verify_message(uintptr_t addr, uint32_t msg_len)
{
	if (msg_len < sizeof(remote_offload::control::message_header)) {
		return false;
	}

	remote_offload::control::message_header *header = (remote_offload::control::message_header *)addr;

	// Message header is in big endian byte order, so it needs swaped before being checked
	uint32_t msg_id = byte_swap_32((uint32_t)header->msg_id);

	if (msg_id != (uint32_t)remote_offload::control::message_id::client_request) {
		return false;
	}
	return true;
}

__device__ void reverse_buffer(uint8_t *head, uint32_t len)
{
	uint8_t *tail = head + len - 1;
	uint8_t h;
	uint8_t t;
	for (; head < tail; ++head, --tail) {
		h = *head;
		t = *tail;

		*head = t;
		*tail = h;
	}
}

__device__ void process_message(uintptr_t addr, uint32_t msg_len)
{
	uint8_t *data = (uint8_t *)addr;
	size_t data_len = msg_len;

	data += sizeof(remote_offload::control::message_header) + sizeof(uint32_t);
	data_len -= sizeof(remote_offload::control::message_header) + sizeof(uint32_t);

	reverse_buffer(data, data_len);
}

__global__ void gpu_proc(volatile bool *stop_flag,
			 gpu_thread_data *thread_data,
			 const uint32_t max_message_size,
			 const uint32_t max_concurrent_messages)
{
	doca_error_t status;
	doca_comch_gpu_consumer *consumer = thread_data[threadIdx.x].consumer;
	doca_comch_gpu_producer *producer = thread_data[threadIdx.x].producer;
	doca_gpu_buf_arr *buf_arr = thread_data[threadIdx.x].buf_arr;
	uint32_t remote_consumer = thread_data[threadIdx.x].remote_consumer_id;

	inflight_msg_data *inflight_messages = thread_data[threadIdx.x].inflight_messages;
	uint32_t inflight_messages_mask = thread_data[threadIdx.x].inflight_messages_mask;
	uint64_t next_inflight_msg_idx = 0;

	doca_gpu_buf *buf;
	uint32_t mkey;
	uintptr_t addr;
	uint32_t recv_len;
	uint64_t user_msg_id;


	/* Submit initial post_recv buffers */
	for (size_t i = 0; i < max_concurrent_messages; i++) {
		doca_gpu_dev_buf_get_buf(buf_arr, i, &buf);

		doca_gpu_dev_buf_get_mkey(buf, &mkey);
		doca_gpu_dev_buf_get_addr(buf, &addr);

		status = doca_dev_gpu_comch_consumer_post_recv(consumer, mkey, addr, max_message_size);

		if (status != DOCA_SUCCESS) {
			*stop_flag = true;
			break;
		}
	}

	while (!(*stop_flag)) {
		status = doca_dev_gpu_comch_consumer_recv_wait(consumer,
							       &mkey,
							       &addr,
							       &recv_len,
							       nullptr,
							       nullptr,
							       DOCA_GPU_COMCH_WAIT_FLAG_NB);
		if (status == DOCA_SUCCESS) {
			if (!verify_message(addr, recv_len)) {
				/* Invalid message received  - fatal error*/
				*stop_flag = true;
				break;
			}
			process_message(addr, recv_len);

			/* Save the mkey and address of the message we're sending here so we know which buffer has been
			 * sent when we get a completion. */
			inflight_messages[next_inflight_msg_idx].mkey = mkey;
			inflight_messages[next_inflight_msg_idx].addr = addr;

			/* Submit the response to the producer */
			do {
				status = doca_dev_gpu_comch_producer_send(producer,
									  mkey,
									  addr,
									  recv_len,
									  nullptr,
									  0,
									  remote_consumer,
									  next_inflight_msg_idx);
			} while (status == DOCA_ERROR_AGAIN && !(*stop_flag));

			if (status != DOCA_SUCCESS) {
				*stop_flag = true;
				break;
			}

			next_inflight_msg_idx++;
			next_inflight_msg_idx &= inflight_messages_mask;

		} else if (status != DOCA_ERROR_AGAIN) {
			/* Fatal error has occurred */
			*stop_flag = true;
			break;
		}

		status = doca_dev_gpu_comch_producer_poll(producer, &user_msg_id, DOCA_GPU_COMCH_WAIT_FLAG_NB);
		if (status == DOCA_SUCCESS) {
			/* Resubmit the buffer that has just been sent for reuse */
			status = doca_dev_gpu_comch_consumer_post_recv(consumer,
								       inflight_messages[user_msg_id].mkey,
								       inflight_messages[user_msg_id].addr,
								       max_message_size);

			if (status != DOCA_SUCCESS) {
				*stop_flag = true;
				break;
			}
		} else if (status != DOCA_ERROR_AGAIN) {
			/* Fatal error has occurred */
			*stop_flag = true;
			break;
		}
	}
}

extern "C" {

doca_error_t start_gpu_processing(uint32_t num_threads,
				  bool *gpu_stop_flag,
				  gpu_thread_data *thread_data,
				  const uint32_t max_message_size,
				  const uint32_t max_concurrent_messages)
{
	gpu_proc<<<1, num_threads, 0>>>(gpu_stop_flag,
					thread_data,
					max_message_size,
					max_concurrent_messages);

	return DOCA_SUCCESS;
}
}
