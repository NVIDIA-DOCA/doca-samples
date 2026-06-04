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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>

#include <log_common.h>

DOCA_LOG_REGISTER(LOG_LIMITS_SERVER);

/* Control value to exit listening loop - triggered by ctrl+c interrupt */
static volatile int end_listening = 0;

/*
 * Signal function to catch ctrl+c and break listening loop
 *
 * @unused [in]: Ignored parameter
 */
static void sigint_handler(int unused)
{
	(void)unused;
	end_listening = 1;
}

static doca_error_t handle_get_limits_command(char *resp_buf,
					      size_t resp_size,
					      struct sockaddr_in *reply_addr,
					      socklen_t addrlen,
					      int sock_fd)
{
	uint32_t lower, upper;
	int n;
	ssize_t m;

	/* Validate parameters */
	if (resp_buf == NULL) {
		printf("Failed to handle %s command: parameter resp_buf=NULL\n",
		       log_command_to_string(LOG_COMMAND_GET_LIMITS));
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (resp_size <= 0) {
		printf("Failed to handle %s command: invalid parameter resp_size\n",
		       log_command_to_string(LOG_COMMAND_GET_LIMITS));
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (reply_addr == NULL) {
		printf("Failed to handle %s command: parameter reply_addr=NULL\n",
		       log_command_to_string(LOG_COMMAND_GET_LIMITS));
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (addrlen == 0) {
		printf("Failed to handle %s command: parameter addrlen=0\n",
		       log_command_to_string(LOG_COMMAND_GET_LIMITS));
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (sock_fd < 0) {
		printf("Failed to handle %s command: parameter sock_fd<0\n",
		       log_command_to_string(LOG_COMMAND_GET_LIMITS));
		return DOCA_ERROR_INVALID_VALUE;
	}

	lower = doca_log_level_get_global_lower_limit();
	upper = doca_log_level_get_global_upper_limit();
	n = snprintf(resp_buf,
		     resp_size,
		     "DOCA_LOG server level: lower limit [%u] (%s), upper limit [%u] (%s)\n",
		     lower,
		     log_level_to_string(lower),
		     upper,
		     log_level_to_string(upper));
	if (n <= 0 || (size_t)n >= resp_size) {
		printf("Failed to handle %s command: snprintf returned %d, resp_size: %zu\n",
		       log_command_to_string(LOG_COMMAND_GET_LIMITS),
		       n,
		       resp_size);
		return DOCA_ERROR_INVALID_VALUE;
	}
	m = sendto(sock_fd, resp_buf, (size_t)n, 0, (struct sockaddr *)reply_addr, addrlen);
	if (m != (ssize_t)n) {
		if (m < 0) {
			printf("Failed to handle %s command: sendto() failed: %s\n",
			       log_command_to_string(LOG_COMMAND_GET_LIMITS),
			       strerror(errno));
		} else {
			printf("Failed to handle %s command: sendto() returned %ld, expected %ld\n",
			       log_command_to_string(LOG_COMMAND_GET_LIMITS),
			       (long)m,
			       (long)n);
		}
		return DOCA_ERROR_IO_FAILED;
	}
	printf("Successfully handled %s command: Response sent: %s\n",
	       log_command_to_string(LOG_COMMAND_GET_LIMITS),
	       resp_buf);
	return DOCA_SUCCESS;
}

/*
 * Handle set limit command
 *
 * @req [in]: Request string
 * @req_len [in]: Request length
 * @is_lower_limit [in]: Whether the lower_limit or the upper_limit should be set
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_set_limit_command(const char *req, size_t req_len, bool is_lower_limit)
{
	const char *cmd_str;
	size_t cmd_str_len;
	size_t limit_value_start_index;
	uint32_t limit_value;
	doca_error_t result;

	if (is_lower_limit) {
		cmd_str = log_command_to_string(LOG_COMMAND_SET_LOWER_LIMIT);
	} else {
		cmd_str = log_command_to_string(LOG_COMMAND_SET_UPPER_LIMIT);
	}
	cmd_str_len = strlen(cmd_str);
	limit_value_start_index = cmd_str_len + 1;

	if (req_len >= limit_value_start_index) {
		limit_value = (uint32_t)strtoul(req + limit_value_start_index, NULL, 10);
		if (!is_log_level_valid(limit_value)) {
			printf("Failed to handle %s command: limit value is invalid: %u\n", req, limit_value);
			return DOCA_ERROR_INVALID_VALUE;
		}

		if (is_lower_limit) {
			result = doca_log_level_set_global_lower_limit(limit_value);
		} else {
			result = doca_log_level_set_global_upper_limit(limit_value);
		}
		if (result != DOCA_SUCCESS) {
			printf("Failed to handle %s command: doca_log_level_set_global_%s_limit returned %s\n",
			       req,
			       is_lower_limit ? "lower" : "upper",
			       doca_error_get_name(result));
			return result;
		}
		printf("Successfully handled %s command: DOCA_LOG server level: lower limit [%u], upper limit [%u]\n",
		       req,
		       doca_log_level_get_global_lower_limit(),
		       doca_log_level_get_global_upper_limit());
		return DOCA_SUCCESS;
	}
	printf("Failed to handle %s command: limit value is not found\n", req);
	return DOCA_ERROR_INVALID_VALUE;
}

/* Parse UDP command: match prefix and length (set-* commands require trailing argument).
 * Only the first matching command is processed.
 *
 * @req [in]: Request string
 * @req_len [in]: Request length
 * @command [out]: Command
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t parse_udp_command(const char *req, size_t req_len, struct command_entry *command)
{
	struct command_entry cmd_arr[] = {
		{LOG_COMMAND_GET_LIMITS, log_command_to_string(LOG_COMMAND_GET_LIMITS)},
		{LOG_COMMAND_SET_LOWER_LIMIT, log_command_to_string(LOG_COMMAND_SET_LOWER_LIMIT)},
		{LOG_COMMAND_SET_UPPER_LIMIT, log_command_to_string(LOG_COMMAND_SET_UPPER_LIMIT)},
	};
	size_t i;
	const char *cmd_str;
	size_t cmd_str_len;

	if (req == NULL) {
		printf("Failed to parse UDP command: parameter req=NULL\n");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (req_len == 0) {
		printf("Failed to parse UDP command: parameter req_len==0\n");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (command == NULL) {
		printf("Failed to parse UDP command: parameter command=NULL\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	command->operation = LOG_COMMAND_NONE;
	command->operation_name = log_command_to_string(LOG_COMMAND_NONE);

	for (i = 0; i < sizeof(cmd_arr) / sizeof(cmd_arr[0]); i++) {
		cmd_str = cmd_arr[i].operation_name;
		cmd_str_len = strlen(cmd_str);
		if (req_len >= cmd_str_len && strncmp(req, cmd_str, cmd_str_len) == 0) {
			command->operation = cmd_arr[i].operation;
			command->operation_name = cmd_arr[i].operation_name;
			break;
		}
	}
	return DOCA_SUCCESS;
}

/*
 * Parse and execute a single UDP command.
 * Request format is a string:
 * ---- "get-limits"
 * ---- "set-lower-limit limit_value"
 * ---- "set-upper-limit limit_value"
 * Response for GET is written into resp_buf (up to resp_size bytes); reply_addr/addrlen used for sendto.
 * @req [in]: Request string
 * @req_len [in]: Request length
 * @resp_buf [out]: Response buffer
 * @resp_size [in]: Response buffer size
 * @reply_addr [in]: Reply address
 * @addrlen [in]: Address length
 * @sock_fd [in]: Socket file descriptor
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t handle_udp_command(const char *req,
				       size_t req_len,
				       char *resp_buf,
				       size_t resp_size,
				       struct sockaddr_in *reply_addr,
				       socklen_t addrlen,
				       int sock_fd)
{
	struct command_entry cmd = {0};
	doca_error_t result;

	/* Validate parameters */
	if (req == NULL) {
		printf("Failed to handle UDP command: parameter req=NULL\n");
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (req_len <= 0) {
		printf("Failed to handle UDP command: invalid parameter req_len\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	printf("\nReceived UDP command: %s", req);

	result = parse_udp_command(req, req_len, &cmd);
	if (result != DOCA_SUCCESS) {
		printf("Failed to parse UDP command: %s\n", doca_error_get_name(result));
		return result;
	}

	switch (cmd.operation) {
	case LOG_COMMAND_GET_LIMITS:
		return handle_get_limits_command(resp_buf, resp_size, reply_addr, addrlen, sock_fd);
	case LOG_COMMAND_SET_LOWER_LIMIT:
		return handle_set_limit_command(req, req_len, true);
	case LOG_COMMAND_SET_UPPER_LIMIT:
		return handle_set_limit_command(req, req_len, false);
	default:
		printf("Failed to handle UDP command: unknown command: %s\n", req);
		return DOCA_ERROR_INVALID_VALUE;
	}
}

/* Test all DOCA_LOG levels. */
static void test_doca_log_levels(void)
{
	enum doca_log_level level;

	printf("\nDOCA_LOG server level: lower limit [%u], upper limit [%u]\n",
	       doca_log_level_get_global_lower_limit(),
	       doca_log_level_get_global_upper_limit());
	level = DOCA_LOG_LEVEL_DISABLE;
	printf("Testing DOCA_LOG level: %d (%s)\n", level, log_level_to_string(level));
	DOCA_LOG(level, "----> Testing DOCA_LOG level %u (%s) ... done", level, log_level_to_string(level));
	level = DOCA_LOG_LEVEL_CRIT;
	printf("Testing DOCA_LOG level: %d (%s)\n", level, log_level_to_string(level));
	DOCA_LOG_CRIT("----> Testing DOCA_LOG level %u (%s) ... done", level, log_level_to_string(level));
	level = DOCA_LOG_LEVEL_ERROR;
	printf("Testing DOCA_LOG level: %d (%s)\n", level, log_level_to_string(level));
	DOCA_LOG_ERR("----> Testing DOCA_LOG level %u (%s) ... done", level, log_level_to_string(level));
	level = DOCA_LOG_LEVEL_WARNING;
	printf("Testing DOCA_LOG level: %d (%s)\n", level, log_level_to_string(level));
	DOCA_LOG_WARN("----> Testing DOCA_LOG level %u (%s) ... done", level, log_level_to_string(level));
	level = DOCA_LOG_LEVEL_INFO;
	printf("Testing DOCA_LOG level: %d (%s)\n", level, log_level_to_string(level));
	DOCA_LOG_INFO("----> Testing DOCA_LOG level %u (%s) ... done", level, log_level_to_string(level));
	level = DOCA_LOG_LEVEL_DEBUG;
	printf("Testing DOCA_LOG level: %d (%s)\n", level, log_level_to_string(level));
	DOCA_LOG_DBG("----> Testing DOCA_LOG level %u (%s) ... done", level, log_level_to_string(level));
	level = DOCA_LOG_LEVEL_TRACE;
	printf("Testing DOCA_LOG level: %d (%s)\n", level, log_level_to_string(level));
	DOCA_LOG_TRC("----> Testing DOCA_LOG level %u (%s) ... done", level, log_level_to_string(level));
}

/**
 * Sample's Logic
 *
 * @cfg [in]: Log sample configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t run_log_limits_server(const struct log_sample_config *cfg)
{
	doca_error_t result = DOCA_SUCCESS;
	char recv_buf[DEFAULT_UDP_RECV_BUF_SIZE];
	char send_buf[DEFAULT_UDP_SEND_BUF_SIZE];
	int ret;
	int sock_fd;
	ssize_t n;
	struct pollfd pfd;
	struct sockaddr_in addr = {0};
	struct sockaddr_in reply_addr;
	socklen_t addrlen = sizeof(reply_addr);
	time_t last_test_time;

	/* Validate parameters */
	if (cfg == NULL) {
		printf("Failed to run log limits server: parameter cfg=NULL\n");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Create socket */
	sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock_fd < 0) {
		DOCA_LOG_ERR("Failed to create socket: %s", strerror(errno));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	DOCA_LOG_INFO("Socket file descriptor %d created successfully", sock_fd);

	/* Bind socket to port */
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(cfg->server_port);
	ret = bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		DOCA_LOG_ERR("Failed to bind (port=%u) to socket: %s", cfg->server_port, strerror(errno));
		close(sock_fd);
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	DOCA_LOG_INFO("Socket bound to port %d successfully", cfg->server_port);
	DOCA_LOG_INFO("DOCA_LOG server level after startup: lower limit [%u], upper limit [%u]",
		      doca_log_level_get_global_lower_limit(),
		      doca_log_level_get_global_upper_limit());
	DOCA_LOG_INFO("DOCA_LOG limits server listening on UDP port %u now ...", cfg->server_port);

	/* Register signal handler for ctrl+c */
	signal(SIGINT, sigint_handler);

	/*
	 * Listen for commands until ctrl+c is received
	 * Because the DOCA_LOG level is changed during the loop, so we need to 'printf' in this loop to make sure all
	 * kinds of messages are printed.
	 */
	last_test_time = time(NULL);
	while (end_listening == 0) {
		pfd.fd = sock_fd;
		pfd.events = POLLIN;
		ret = poll(&pfd, 1, DEFAULT_SERVER_LISTENING_POLLING_TIMEOUT_MS);
		if (ret > 0 && (pfd.revents & POLLIN)) {
			addrlen = sizeof(reply_addr);
			n = recvfrom(sock_fd,
				     recv_buf,
				     sizeof(recv_buf) - 1,
				     0,
				     (struct sockaddr *)&reply_addr,
				     &addrlen);
			if (n < 0) {
				printf("Failed to receive UDP packet: recvfrom() failed: %s\n", strerror(errno));
				continue;
			}
			if (n == 0) {
				continue; /* ignore zero-length datagram */
			}
			recv_buf[n] = '\0';
			result = handle_udp_command(recv_buf,
						    (size_t)n,
						    send_buf,
						    sizeof(send_buf),
						    &reply_addr,
						    addrlen,
						    sock_fd);
			if (result == DOCA_ERROR_IO_FAILED) {
				printf("Connection failure detected, stopping DOCA_LOG limits server...\n");
				/* Only break the loop if the connection failure is detected */
				break;
			}
		}

		/* Test all DOCA_LOG levels, it is used to show whether the log levels changes are working */
		if (time(NULL) - last_test_time >= DEFAULT_SERVER_LISTENING_PRINT_INTERVAL_S) {
			test_doca_log_levels();
			last_test_time = time(NULL);
		}
	}

	close(sock_fd);
	printf("DOCA_LOG limits server stopped\n");
	return result;
}
