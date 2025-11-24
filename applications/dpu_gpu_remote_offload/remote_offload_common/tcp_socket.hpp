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

#ifndef APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_TCP_SOCKET_HPP_
#define APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_TCP_SOCKET_HPP_

#include <cstdint>
#include <string>

namespace remote_offload {

/*
 * TCP socket helper class
 */
class tcp_socket {
public:
	/*
	 * TCP connection status codes
	 */
	enum class connection_status {
		connected = 0, /* Connected */
		establishing,  /* Trying to connect but not done yet */
		refused,       /* Remote refused the connection */
		failed,	       /* Connection failed for some reason */
	};

	/*
	 * Destroy and disconnect the socket
	 */
	~tcp_socket();

	/*
	 * Create a default (invalid) socket
	 */
	tcp_socket() noexcept;

	/*
	 * Create a socket from a given fd number
	 *
	 * @fd [in]: Underlying socket fd number to use
	 *
	 * @throws std::runtime_error: If unable to set socket options
	 */
	explicit tcp_socket(uint32_t fd);

	/*
	 * Disabled copy constructor
	 */
	tcp_socket(tcp_socket const &) = delete;

	/*
	 * Move constructor
	 *
	 * @other [in]: Object to move from
	 */
	tcp_socket(tcp_socket &&other) noexcept;

	/*
	 * Disabled copy assignment operator
	 */
	tcp_socket &operator=(tcp_socket const &) = delete;

	/*
	 * Move assignment operator
	 *
	 * @other [in]: Object to move from
	 * @return: Reference to assigned object
	 */
	tcp_socket &operator=(tcp_socket &&other) noexcept;

	/*
	 * Close the socket
	 */
	void close(void) noexcept;

	/*
	 * Connect the socket to a server at the given address
	 *
	 * @ip_address [in]: Address to connect to
	 * @port [in]: Port to connect to
	 */
	void connect(std::string const &ip_address, uint16_t port);

	/*
	 * Listen for connections on the specified port
	 *
	 * @port [in]: Port to listen on
	 */
	void listen(uint16_t port);

	/*
	 * Poll the socket to update its current state. Call this before:
	 *  - Checking if the socket is connected
	 *  - Reading data
	 *  - Writing data
	 */
	void poll(void) noexcept;

	/* Get the socket connection status.
	 *
	 * @return: The current connection status
	 */
	connection_status get_connection_status() const noexcept;

	/* Is the socket connected or not. If not, you can use get_connection_status to find out why not.
	 *
	 * @return: true if the socket is connected
	 */
	bool is_connected() const noexcept;

	/* Can this socket be read from. DO NOT call read if this returns false.
	 *
	 * @return: true if the socket can be read from
	 */
	bool can_read() const noexcept;

	/* Can this socket be written to from. DO NOT call write if this returns false.
	 *
	 * @return: true if the socket can be written to
	 */
	bool can_write() const noexcept;

	/*
	 * Start accepting incoming connections
	 *
	 * @return: A tcp_socket object. The user must check using is_valid to know if the returned socket is connected
	 * to anything or not
	 */
	tcp_socket accept(void);

	/*
	 * Write bytes to the socket. You should call poll and check can_write before calling this function.
	 *
	 * @buffer [in]: Pointer to an array of bytes
	 * @byte_count [in]: The number of bytes to write from the buffer pointed to by buffer
	 *
	 * @return: When zero or positive: The number of bytes written to the socket
	 *          When negative: writing to the socket has failed and the socket should be destroyed.
	 */
	ssize_t write(uint8_t const *buffer, size_t byte_count) noexcept;

	/*
	 * Read bytes from the socket. You should call poll and check can_read before calling this function. If can_read
	 * returned true and then read returns 0 bytes this signals the remote side has closed the socket.
	 *
	 * @param [in] buffer pointer to an array of bytes
	 * @param [in] buffer_capacity number of bytes available in the array pointed to by buffer
	 *
	 * @return: When zero or positive: The number of bytes read from the socket
	 *          When negative: reading from to the socket has failed and the socket should be destroyed.
	 */
	ssize_t read(uint8_t *buffer, size_t buffer_capacity) noexcept;

	/*
	 * Check if the socket is valid or not, invalid sockets can be simply disposed of at negligible cost
	 *
	 * @return: true if socket is valid (refers to something) otherwise false
	 */
	bool is_valid(void) const noexcept;

private:
	uint32_t m_fd;	  /* internal socket number */
	uint32_t m_flags; /* flags to record socket state */
};

} // namespace remote_offload

#endif /* APPLICATIONS_DPU_GPU_REMOTE_OFFLOAD_REMOTE_OFFLOAD_COMMON_TCP_SOCKET_HPP_ */
