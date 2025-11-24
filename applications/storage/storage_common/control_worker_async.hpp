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

#ifndef APPLICATIONS_STORAGE_STORAGE_COMMON_CONTROL_WORKER_ASYNC_HPP_
#define APPLICATIONS_STORAGE_STORAGE_COMMON_CONTROL_WORKER_ASYNC_HPP_

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include <doca_error.h>

namespace storage::control {

template <typename CmdBaseT>
class worker_async {
public:
	~worker_async() = default;
	worker_async() = default;
	worker_async(worker_async const &) = delete;
	worker_async(worker_async &&) noexcept = delete;
	worker_async &operator=(worker_async const &) = delete;
	worker_async &operator=(worker_async &&) noexcept = delete;

	void lock()
	{
		m_lock.lock();
	}

	void unlock()
	{
		m_lock.unlock();
	}

	void execute_async(CmdBaseT *cmd) noexcept
	{
		m_result = DOCA_ERROR_UNKNOWN;
		m_result_valid = false;
		m_command = cmd;
	}

	CmdBaseT *get_command() noexcept
	{
		return m_command;
	}

	void set_result(doca_error_t result) noexcept
	{
		m_command = nullptr;
		m_result = result;
		m_result_valid = true;
	}

	bool has_result() const noexcept
	{
		return m_result_valid;
	}

	doca_error_t get_result() noexcept
	{
		return m_result;
	}

private:
	std::mutex m_lock{};
	CmdBaseT *m_command{};
	doca_error_t m_result{DOCA_ERROR_UNKNOWN};
	std::atomic_bool m_result_valid{false};
};

template <typename CmdBaseT>
doca_error_t execute_worker_command(worker_async<CmdBaseT> &async, CmdBaseT *cmd, std::chrono::seconds timeout)
{
	async.lock();
	async.execute_async(cmd);
	async.unlock();

	auto const expiry_time = std::chrono::steady_clock::now() + timeout;
	for (;;) {
		async.lock();
		if (async.has_result()) {
			doca_error_t result = async.get_result();
			async.unlock();
			return result;
		}
		async.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds{10});

		if (std::chrono::steady_clock::now() > expiry_time) {
			return DOCA_ERROR_TIME_OUT;
		}
	}
}

} // namespace storage::control

#endif // APPLICATIONS_STORAGE_STORAGE_COMMON_CONTROL_WORKER_ASYNC_HPP_
