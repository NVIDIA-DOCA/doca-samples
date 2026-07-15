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

#include <storage_common/tcp_socket.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <doca_log.h>

#include <storage_common/definitions.hpp>
#include <storage_common/os_utils.hpp>

DOCA_LOG_REGISTER(TCP_SOCKET);

using namespace std::string_literals;

namespace storage {
namespace {
uint32_t constexpr invalid_socket{std::numeric_limits<uint32_t>::max()};

} /* namespace */

tcp_socket::~tcp_socket()
{
	try {
		close();
	} catch (std::runtime_error const &ex) {
		DOCA_LOG_ERR("Failed to close socket during destruction: %s", ex.what());
	}
}

tcp_socket::tcp_socket() : m_fd{invalid_socket}
{
	auto ret = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (ret < 0) {
		throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
					     "Failed to create socket. Error: " + storage::strerror_r(errno)};
	}

	m_fd = static_cast<uint32_t>(ret);
	set_socket_options();
}

tcp_socket::tcp_socket(uint32_t fd) : m_fd{fd}
{
	if (fd != invalid_socket) {
		// Only do this when not creating an "empty" socket
		set_socket_options();
	}
}

tcp_socket::tcp_socket(tcp_socket &&other) noexcept : m_fd{other.m_fd}
{
	other.m_fd = invalid_socket;
}

tcp_socket &tcp_socket::operator=(tcp_socket &&other) noexcept
{
	if (std::addressof(other) == this)
		return *this;

	m_fd = other.m_fd;
	other.m_fd = invalid_socket;

	return *this;
}

void tcp_socket::set_blocking(bool blocking)
{
	auto flags = fcntl(static_cast<int>(m_fd), F_GETFL);
	if (flags == -1) {
		throw storage::runtime_error{
			DOCA_ERROR_OPERATING_SYSTEM,
			"Failed to get current blocking mode. Error: " + storage::strerror_r(errno) + "\n"};
	}
	if (blocking) {
		flags &= ~O_NONBLOCK;
	} else {
		flags |= O_NONBLOCK;
	}
	auto ret = fcntl(static_cast<int>(m_fd), F_SETFL, flags);
	if (ret == -1) {
		throw storage::runtime_error{
			DOCA_ERROR_OPERATING_SYSTEM,
			"Failed to set new current blocking mode. Error: " + storage::strerror_r(errno) + "\n"};
	}
}

void tcp_socket::close(void)
{
	if (m_fd == invalid_socket)
		return;

	DOCA_LOG_DBG("Close socket %u", m_fd);
	auto const ret = ::close(static_cast<int>(m_fd));
	if (ret != 0) {
		throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
					     "Failed to close. Error: " + storage::strerror_r(errno) + "\n"};
	}

	m_fd = invalid_socket;
}

void tcp_socket::connect(storage::ip_address const &address)
{
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(address.get_port());

	if (::inet_pton(AF_INET, address.get_address().c_str(), &sa.sin_addr) < 0) {
		throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
					     "Failed to parse address: \"" + address.get_address() +
						     "\":" + std::to_string(address.get_port()) +
						     ". Error: " + storage::strerror_r(errno)};
	}

	if (::connect(static_cast<int>(m_fd), reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
		// https://man7.org/linux/man-pages/man2/connect.2.html
		auto const err = errno;
		switch (err) {
		case EINPROGRESS: //  The socket is nonblocking and the connection cannot be completed
				  //  immediately. It is possible to select(2) or poll(2) for completion by
				  //  selecting the socket for writing.  After  select(2) indicates writability,
				  //  use getsockopt(2) to read the SO_ERROR option at level SOL_SOCKET to
				  //  determine whether connect() completed successfully (SO_ERROR is zero) or
				  //  unsuccessfully (SO_ERROR is one of the usual error codes listed here,
				  //  explaining the reason for the failure)
			break;
		default:
			throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
						     "Failed to connect to: \"" + address.get_address() +
							     "\":" + std::to_string(address.get_port()) +
							     ". Error: " + storage::strerror_r(errno)};
		}
	}
}

void tcp_socket::listen(uint16_t port)
{
	sockaddr_in sa;
	::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);

	DOCA_LOG_DBG("Listening on socket %u", m_fd);

	if (::bind(static_cast<int>(m_fd), reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
		throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
					     "Failed to bind to port: " + std::to_string(port) +
						     ". Error: " + storage::strerror_r(errno)};
	}

	if (::listen(static_cast<int>(m_fd), 1) != 0) {
		throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
					     "Failed to listen on port: " + std::to_string(port) +
						     ". Error: " + storage::strerror_r(errno)};
	}
}

tcp_socket::connection_status tcp_socket::poll_is_connected()
{
	pollfd pfd{};

	pfd.fd = static_cast<int>(m_fd);
	pfd.events = POLLOUT;
	pfd.revents = 0;

	auto ret = poll(&pfd, 1, 0);

	if (ret < 0) {
		return connection_status::failed;
	}

	if (ret == 0)
		return connection_status::establishing;

	if (pfd.revents & POLLOUT) {
		socklen_t ret_size = sizeof(ret);
		if (::getsockopt(static_cast<int>(m_fd), SOL_SOCKET, SO_ERROR, &ret, &ret_size) != 0) {
			return connection_status::failed;
		}

		if (ret == ECONNABORTED || ret == ECONNREFUSED || ret == ECONNRESET || ret == ENOTCONN) {
			return connection_status::refused;
		}

		if (ret == 0) {
			struct sockaddr_in peeraddr {};
			socklen_t peeraddrlen{};
			ret = getpeername(static_cast<int>(m_fd), (struct sockaddr *)&peeraddr, &peeraddrlen);
			if (ret == 0) {
				DOCA_LOG_DBG("Connected socket %u to: %s:%u",
					     m_fd,
					     inet_ntoa(peeraddr.sin_addr),
					     ntohs(peeraddr.sin_port));
				return connection_status::connected;
			}

			auto const err = errno;
			if (err == ENOTCONN)
				return connection_status::establishing;

			return connection_status::failed;
		}
	}

	return connection_status::establishing;
}

tcp_socket tcp_socket::accept()
{
	sockaddr_storage ss{};
	socklen_t peer_addr_len = sizeof(ss);
	auto const client_fd =
		::accept4(static_cast<int>(m_fd), reinterpret_cast<sockaddr *>(&ss), &peer_addr_len, SOCK_NONBLOCK);

	if (client_fd < 0) {
		// https://man7.org/linux/man-pages/man2/accept.2.html
		auto const err = errno;
		switch (err) {
		case EAGAIN: // The socket is marked nonblocking and no connections are present to be accepted
#if EWOULDBLOCK != EAGAIN
		// POSIX.1-2001 allows either error to be returned for this case, and does not require these
		// constants to  have the same value, so a portable application should check for both
		// possibilities
		case EWOULDBLOCK:
#endif
			return tcp_socket{invalid_socket};
		default:
			storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
					       "Failed to accept new connection. Error: " + storage::strerror_r(err)};
		}
	}

	DOCA_LOG_DBG("Accepted socket %u", client_fd);
	return tcp_socket{static_cast<uint32_t>(client_fd)};
}

size_t tcp_socket::write(char const *buffer, size_t byte_count)
{
	auto const ret = ::write(static_cast<int>(m_fd), buffer, byte_count);
	if (ret < 0) {
		auto const err = errno;
		// https://man7.org/linux/man-pages/man2/write.2.html
		switch (err) {
		case EAGAIN: // The file descriptor fd refers to a file other than a socket and has been
			     // marked nonblocking (O_NONBLOCK), and the write would block
#if EWOULDBLOCK != EAGAIN
		// POSIX.1-2001 allows either error to be returned for this case, and does not require
		// these constants to  have the same value, so a portable application should check for
		// both possibilities
		case EWOULDBLOCK:
#endif			    // DEBUG_TCP_SOCKET_OPS
		case EINTR: // The call was interrupted by a signal before any data was written
			return 0;
		default:
			DOCA_LOG_DBG("Write to socket %u failed: %s", m_fd, storage::strerror_r(err).c_str());
			storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
					       "Failed to write data. Error: " + storage::strerror_r(err)};
		}
	}

	if (ret > 0) {
		DOCA_LOG_DBG("Tx %ld bytes", ret);
	}

	return static_cast<size_t>(ret);
}

size_t tcp_socket::read(char *buffer, size_t buffer_capacity)
{
	auto const ret = ::read(static_cast<int>(m_fd), buffer, buffer_capacity);
	if (ret < 0) {
		auto const err = errno;
		// https://man7.org/linux/man-pages/man2/read.2.html
		switch (err) {
		case EAGAIN: //  The file descriptor fd refers to a file other than a socket and has
			     //  been marked nonblocking (O_NONBLOCK), and the read would block
#if EWOULDBLOCK != EAGAIN
		// POSIX.1-2001 allows either error to be returned for this case, and does not require
		// these constants to  have the same value, so a portable application should check for
		// both possibilities
		case EWOULDBLOCK:
#endif			    // DEBUG_TCP_SOCKET_OPS
		case EINTR: // The call was interrupted by a signal before any data was read
			return {};
		default:
			DOCA_LOG_DBG("Read from socket %u failed: %s\n", m_fd, storage::strerror_r(err).c_str());
			throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
						     "Failed to read data. Error: " + storage::strerror_r(err)};
		}
	}

	if (ret > 0) {
		DOCA_LOG_DBG("Rx %ld bytes", ret);
	}

	return static_cast<size_t>(ret);
}

bool tcp_socket::is_valid(void) const noexcept
{
	return m_fd != invalid_socket;
}

void tcp_socket::set_socket_options(void)
{
	int ret;

	{
		linger linger_options{
			0, // no linger
			0  // 0 seconds
		};

		ret = ::setsockopt(static_cast<int>(m_fd),
				   SOL_SOCKET,
				   SO_LINGER,
				   &linger_options,
				   sizeof(linger_options));
		if (ret != 0) {
			throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
						     "Failed to set socket option SO_LINGER. Error: \""s +
							     ::gai_strerror(errno) + "\""};
		}
	}
	{
		int const reuse_addr{1};

		ret = ::setsockopt(static_cast<int>(m_fd), SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
		if (ret != 0) {
			throw storage::runtime_error{DOCA_ERROR_OPERATING_SYSTEM,
						     "Failed to set socket option SO_REUSEADDR. Error: \""s +
							     ::gai_strerror(errno) + "\""};
		}
	}
}

} // namespace storage