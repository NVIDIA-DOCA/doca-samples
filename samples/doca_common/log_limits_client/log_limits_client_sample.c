/*
 * Copyright (c) 2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>

#include <log_common.h>

DOCA_LOG_REGISTER(LOG_LIMITS_CLIENT);

/*
 * Build UDP request string from cfg->command
 * The request string is written into buf with the size of buf_size.
 *
 * @cfg [in]: Log sample configuration
 * @buf [out]: Buffer to write the request string
 * @buf_size [in]: Size of the buffer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t build_request(const struct log_sample_config *cfg, char *buf, size_t buf_size)
{
	int n = 0;

	/* Validate parameters */
	if (cfg == NULL) {
		DOCA_LOG_ERR("Failed to build request: parameter cfg=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (buf == NULL) {
		DOCA_LOG_ERR("Failed to build request: parameter buf=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (buf_size == 0) {
		DOCA_LOG_ERR("Failed to build request: invalid buffer size");
		return DOCA_ERROR_INVALID_VALUE;
	}

	switch (cfg->cmd.operation) {
	case LOG_COMMAND_GET_LIMITS: {
		n = snprintf(buf, buf_size, "%s\n", cfg->cmd.operation_name);
		break;
	}
	case LOG_COMMAND_SET_LOWER_LIMIT:
	case LOG_COMMAND_SET_UPPER_LIMIT: {
		n = snprintf(buf, buf_size, "%s %u\n", cfg->cmd.operation_name, cfg->cmd_arg);
		break;
	}
	default: {
		DOCA_LOG_ERR("Failed to build request: unknown command: %s", log_command_to_string(cfg->cmd.operation));
		return DOCA_ERROR_INVALID_VALUE;
	}
	}
	return (n > 0 && (size_t)n < buf_size) ? DOCA_SUCCESS : DOCA_ERROR_INVALID_VALUE;
}

/*
 * Sample's Logic
 *
 * @cfg [in]: Log sample configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t run_log_limits_client(const struct log_sample_config *cfg)
{
	doca_error_t result = DOCA_SUCCESS;
	char recv_buf[DEFAULT_UDP_RECV_BUF_SIZE];
	char send_buf[DEFAULT_UDP_SEND_BUF_SIZE];
	int poll_result;
	int sock_fd;
	ssize_t n;
	struct sockaddr_in server_addr;
	struct pollfd pfd;

	/* Validate parameters */
	if (cfg == NULL) {
		DOCA_LOG_ERR("Failed to run log limits client: parameter cfg=NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (!cfg->cmd_set) {
		DOCA_LOG_ERR(
			"Failed to run log limits client: a valid command (get-limits, set-lower-limit, set-upper-limit) is required");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (cfg->server_ip[0] == '\0') {
		DOCA_LOG_ERR(
			"Failed to run log limits client: a valid server IPv4 address is required (e.g. 127.0.0.1)");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Build UDP request string from cfg->command; write into send_buf (size send_buf_size) */
	result = build_request(cfg, send_buf, sizeof(send_buf));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to build client request: %s", doca_error_get_name(result));
		return result;
	}
	DOCA_LOG_INFO("Successfully built client request: %s", send_buf);

	/* Create socket */
	sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock_fd < 0) {
		DOCA_LOG_ERR("Failed to create socket: %s", strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	DOCA_LOG_INFO("Successfully created socket file descriptor %d", sock_fd);

	/* Set server address and port */
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(cfg->server_port);
	if (inet_pton(AF_INET, cfg->server_ip, &server_addr.sin_addr) <= 0) {
		DOCA_LOG_ERR("Failed to set server address and port: %s", strerror(errno));
		result = DOCA_ERROR_OPERATING_SYSTEM;
		goto close_sock_fd;
	}
	DOCA_LOG_INFO("Successfully set server address and port %s:%u", cfg->server_ip, cfg->server_port);

	/* Send request to server */
	n = sendto(sock_fd, send_buf, strlen(send_buf), 0, (struct sockaddr *)&server_addr, sizeof(server_addr));
	if (n < 0 || (size_t)n != strlen(send_buf)) {
		DOCA_LOG_ERR("Failed to send command to server: %s", strerror(errno));
		result = DOCA_ERROR_OPERATING_SYSTEM;
		goto close_sock_fd;
	}
	DOCA_LOG_INFO("Successfully sent command [%s] to server", cfg->cmd.operation_name);

	/* Wait for response from server */
	if (cfg->cmd.operation == LOG_COMMAND_GET_LIMITS) {
		pfd.fd = sock_fd;
		pfd.events = POLLIN;
		result = DOCA_ERROR_UNKNOWN;

		poll_result = poll(&pfd, 1, DEFAULT_CLIENT_RESPONSE_TIMEOUT_MS);
		if (poll_result > 0 && (pfd.revents & POLLIN)) {
			n = recvfrom(sock_fd, recv_buf, sizeof(recv_buf) - 1, 0, NULL, NULL);
			if (n > 0) {
				recv_buf[n] = '\0';
				DOCA_LOG_INFO("Successfully received response from server: %s", recv_buf);
				result = DOCA_SUCCESS;
			}
		}

		if (result != DOCA_SUCCESS) {
			if (poll_result == 0) {
				DOCA_LOG_ERR("Failed to receive response from server: timeout");
				result = DOCA_ERROR_TIME_OUT;
			} else {
				DOCA_LOG_ERR("Failed to receive response from server: %s", strerror(errno));
				result = DOCA_ERROR_OPERATING_SYSTEM;
			}
		}
	}

close_sock_fd:
	close(sock_fd);
	DOCA_LOG_INFO("Successfully closed socket file descriptor %d", sock_fd);
	return result;
}
