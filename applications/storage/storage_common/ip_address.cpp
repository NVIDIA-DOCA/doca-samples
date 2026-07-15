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

#include <storage_common/ip_address.hpp>

#include <cstring>
#include <stdexcept>
#include <string>

using namespace std::string_literals;

namespace storage {

ip_address::ip_address(std::string address, uint16_t port) : m_addr{std::move(address)}, m_port{port}
{
}

std::string const &ip_address::get_address() const noexcept
{
	return m_addr;
}

uint16_t ip_address::get_port() const noexcept
{
	return m_port;
}

ip_address parse_ip_v4_address(char const *value)
{
	auto const *const end = value + ::strnlen(value, 22);
	std::string addr;
	uint32_t port = 0;

	uint8_t dots = 0;
	for (auto *iter = value; iter != end; ++iter) {
		if (std::isspace(*iter))
			throw std::runtime_error{"Spaces are not permitted in IP address strings" +
						 std::to_string(UINT16_MAX)};
		if (*iter == '.') {
			++dots;
		} else if (*iter == ':') {
			if (dots != 3) {
				throw std::runtime_error{"Invalid ip address: \""s + value +
							 "\" expected 4 octets before the port number %u" +
							 std::to_string(UINT16_MAX)};
			}

			addr = std::string{value, iter};
			port = ::strtoul(iter + 1, nullptr, 10);
			if (port > UINT16_MAX) {
				throw std::runtime_error{"Invalid ip address: \""s + value +
							 "\", port number exceeds %u" + std::to_string(UINT16_MAX)};
			}

			return ip_address{std::move(addr), static_cast<uint16_t>(port)};
		}
	}

	throw std::runtime_error{"Invalid ip address: \""s + value + "\""};
}

} /* namespace storage */