/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef APPLICATIONS_STORAGE_ZERO_COPY_COMCH_TO_RDMA_APPLICATION_HPP_
#define APPLICATIONS_STORAGE_ZERO_COPY_COMCH_TO_RDMA_APPLICATION_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include <storage_common/ip_address.hpp>

namespace storage::zero_copy {

class comch_to_rdma_application {
public:
	/*
	 * Application configuration
	 */
	struct configuration {
		std::string device_id;
		std::string representor_id;
		std::string command_channel_name;
		std::vector<uint32_t> cpu_set;
		storage::ip_address storage_server_address;
	};

	/*
	 * Application stats
	 */
	struct thread_stats {
		uint64_t pe_hit_count;
		uint64_t pe_miss_count;
	};

	/*
	 * Default destructor
	 */
	virtual ~comch_to_rdma_application() = default;

	/*
	 * Run the application
	 */
	virtual void run(void) = 0;

	/*
	 * Abort execution
	 *
	 * @reason [in]: The reason
	 */
	virtual void abort(std::string const &reason) = 0;

	/*
	 * Get end of run statistics
	 *
	 * @return: Statistics
	 */
	virtual std::vector<storage::zero_copy::comch_to_rdma_application::thread_stats> get_stats(void) const = 0;
};

/*
 * Create a ComCh to RDMA application instance
 *
 * @throws std::bad_alloc if memory allocation fails
 * @throws std::runtime_error if any other error occurs
 *
 * @cfg [in]: Application configuration
 * @return: Application instance
 */
storage::zero_copy::comch_to_rdma_application *make_comch_to_rdma_application(
	storage::zero_copy::comch_to_rdma_application::configuration const &cfg);

} /* namespace storage::zero_copy */

#endif /* APPLICATIONS_STORAGE_ZERO_COPY_COMCH_TO_RDMA_APPLICATION_HPP_ */