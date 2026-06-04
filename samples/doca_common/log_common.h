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

#ifndef LOG_COMMON_H_
#define LOG_COMMON_H_

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>
#include <doca_log.h>

enum log_command {
	LOG_COMMAND_NONE = 0,
	LOG_COMMAND_GET_LIMITS,
	LOG_COMMAND_SET_LOWER_LIMIT,
	LOG_COMMAND_SET_UPPER_LIMIT,
};

#define DEFAULT_LOG_SERVER_PORT 9999
#define DEFAULT_LOG_SERVER_IP_ADDRESS "127.0.0.1"
#define DEFAULT_UDP_RECV_BUF_SIZE 128
#define DEFAULT_UDP_SEND_BUF_SIZE 128
#define DEFAULT_SERVER_LISTENING_PRINT_INTERVAL_S 2	/* 2 seconds */
#define DEFAULT_SERVER_LISTENING_POLLING_TIMEOUT_MS 200 /* 200 milliseconds */
#define DEFAULT_CLIENT_RESPONSE_TIMEOUT_MS 2000		/* 2000 milliseconds */
#define DEFAULT_MINIMUM_LOG_SERVER_PORT 1025

#define IPV4_ADDR_SIZE ((INET_ADDRSTRLEN) + 1)

/* Command entry struct */
struct command_entry {
	enum log_command operation;
	const char *operation_name;
};

/* Log sample configuration struct */
struct log_sample_config {
	char server_ip[IPV4_ADDR_SIZE]; /* Server IP address, only IPv4 is supported */
	uint16_t server_port;		/* Server port, should be a port number > 1024 to avoid "sudo" requirement */
	struct command_entry cmd;	/* Command to execute (get-limits, set-lower-limit, set-upper-limit) */
	uint32_t cmd_arg; /* Command argument, only used for set-lower-limit and set-upper-limit commands */
	bool cmd_set;	  /* Whether the command is provided by the user */
	bool is_client;	  /* Whether the sample is a client */
};

/*
 * Register the command line parameters for the DOCA log samples
 *
 * @is_client [in]: Indication for handling configuration parameters which are
 * needed when there is a client side
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_log_sample_params(bool is_client);

/*
 * Set the default log sample configuration
 *
 * @cfg [in/out]: Log sample configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t set_default_log_sample_config(struct log_sample_config *cfg);

/*
 * Print the log sample configuration
 *
 * @cfg [in]: Log sample configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t print_log_sample_config(const struct log_sample_config *cfg);

/*
 * Convert a log command enum value to a readable string
 *
 * @cmd [in]: Log command
 * @return: String representation of the log command
 */
const char *log_command_to_string(enum log_command cmd);

/*
 * Convert a log level enum value to a readable string
 *
 * @level [in]: Log level
 * @return: String representation of the log level
 */
const char *log_level_to_string(enum doca_log_level level);

/*
 * Validate the user supplied log level limit value
 *
 * @limit [in]: Log level limit
 * @return: true if the log level limit is valid, false otherwise
 */
bool is_log_level_valid(uint32_t limit);

#endif /* LOG_COMMON_H_ */
