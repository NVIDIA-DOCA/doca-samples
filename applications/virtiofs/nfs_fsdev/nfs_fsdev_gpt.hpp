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

/* GPT = Global Per Thread */
#ifndef NFS_FSDEV_GPT_HPP
#define NFS_FSDEV_GPT_HPP

#include <unordered_map>
#include <tuple>
#include <utility>

/**
 * @brief IoDevice provides per-thread storage for device data.
 * @tparam T1 Type of global data.
 * @tparam T2 Type of per-thread data.
 */
template <typename T1, typename T2>
class IoDevice {
public:
	IoDevice()
	{
	}

	template <typename... Args>
	explicit IoDevice(Args &&...args) : global_data(std::forward<Args>(args)...)
	{
	}

	T1 global_data;
	static thread_local std::unordered_map<char *, T2> threadLocalVars;

	/**
	 * @brief Get or create per-thread data for this device.
	 * @return Reference to the per-thread data.
	 */
	T2 &getPerThreadData()
	{
		if (threadLocalVars.find((char *)this) == threadLocalVars.end()) {
			threadLocalVars.emplace(std::piecewise_construct,
						std::forward_as_tuple((char *)this),
						std::forward_as_tuple(&global_data));
		}
		return threadLocalVars[(char *)this];
	}
};

template <typename T1, typename T2>
thread_local std::unordered_map<char *, T2> IoDevice<T1, T2>::threadLocalVars;

#endif
