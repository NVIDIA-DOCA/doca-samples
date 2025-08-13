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

#ifndef CMD_H
#define CMD_H

#include <getopt.h>
#include <stdbool.h>

#include "util.h"

struct cmd {
	const char *name;
	const char *help;
	int (*func)(int argc, char *argv[], struct cmd *cmd);
};

enum cmd_mode_device_req {
	CMD_MODE_DEVICE_REQ_YES,
	CMD_MODE_DEVICE_REQ_NO,
	CMD_MODE_DEVICE_REQ_OPTIONAL,
};

struct cmd_mode {
	const char *name;
	const char *help;
	int (*func)(int argc, char *argv[], struct cmd_mode *mode);
	enum cmd_mode_device_req device_req;
	bool default_mode;
};

enum cmd_opt_type {
	CMD_OPT_TYPE_FLAG,
	CMD_OPT_TYPE_INC,
	CMD_OPT_TYPE_BOOL,
};

struct cmd_opt {
	const char *long_opt;
	char short_opt;
	enum cmd_opt_type type;
	const char *type_help;
	void *value;
	int has_arg;
	char *help;
	bool is_set;
	bool required;
};

/**
 * cmd_opt_is_set:
 * @opts: List of available options
 * @short_opt: Short option name to check if it is set
 *
 * Checks if a given option was set by the user.
 *
 * Returns true if the option is set, false otherwise.
 */
bool cmd_opt_is_set(struct cmd_opt *opts, char short_opt);

int cmd_version(int argc, char *argv[], struct cmd *cmd);

#define OPT_ENTRY_COMMON(l, s, t, th, v, h, r)				       \
	{								       \
		.long_opt = l,						       \
		.short_opt = s,						       \
		.type = t,						       \
		.type_help = th,					       \
		.value = v,						       \
		.has_arg = required_argument,				       \
		.help = h,						       \
		.required = r,						       \
	}

/**
 * OPT_ENTRY_FLAG:
 * @l: Long option name
 * @s: Short option name
 * @v: Pointer to a boolean variable that will hold the option value
 * @h: Short description of the option
 *
 * Defines a flag option entry in the option list of a tool command mode.
 *
 * The flag option doesn't take arguments. If the option is present in the
 * command line arguments, the variable pointed to by @v will be set to true.
 */
#define OPT_ENTRY_FLAG(l, s, v, h)					       \
	{								       \
		.long_opt = l,						       \
		.short_opt = s,						       \
		.type = CMD_OPT_TYPE_FLAG,				       \
		.value = v,						       \
		.has_arg = no_argument,					       \
		.help = h,						       \
		.required = false,					       \
	}

/**
 * OPT_ENTRY_JSON:
 * @v: Pointer to a boolean variable that will hold the option value
 *
 * Defines a json option entry in the option list of a tool command mode.
 *
 * The json option is a convenient wrapper of a flag option that doesn't take
 * arguments. If the option is present in the command line arguments, the
 * variable pointed to by @v will be set to true, which indicates that the
 * output should be printed in JSON format.
 */
#define OPT_ENTRY_JSON(v)						       \
	OPT_ENTRY_FLAG("json", 'j', v, "Print output in JSON format")

/**
 * OPT_ENTRY_INC:
 * @l: Long option name
 * @s: Short option name
 * @v: Pointer to an int variable that will hold the option value
 * @h: Short description of the option
 *
 * Defines an incremental option entry in the option list of a tool command
 * mode.
 *
 * The incremental option doesn't take arguments. For each occurrence of it in
 * the command line arguments, the variable pointed to by @v is incremented.
 */
#define OPT_ENTRY_INC(l, s, v, h)					       \
	{								       \
		.long_opt = l,						       \
		.short_opt = s,						       \
		.type = CMD_OPT_TYPE_INC,				       \
		.value = v,						       \
		.has_arg = no_argument,					       \
		.help = h,						       \
		.required = false,					       \
	}

/**
 * OPT_ENTRY_BOOL:
 * @l: Long option name
 * @s: Short option name
 * @v: Pointer to a boolean variable that will hold the option value
 * @h: Short description of the option
 * @r: Whether the option is required
 *
 * Defines a boolean option entry in the option list of a tool command mode.
 *
 * The boolean option takes a single argument: true, false, 1 or 0. If the
 * option is present in the command line arguments, the variable pointed to by
 * @v will be set to true if the argument is true or 1, and will be set to false
 * if the argument is false or 0.
 *
 * If @r is true and the option is not present in the command line arguments,
 * option parsing will fail with an appropriate error.
 */
#define OPT_ENTRY_BOOL(l, s, v, h, r)                                          \
	OPT_ENTRY_COMMON(l, s, CMD_OPT_TYPE_BOOL, "BOOL", v, h, r)

#define OPT_ENTRY_END() { 0 }

/**
 * OPT_LIST:
 * @n: Name of the variable that holds the command mode option list
 * @...: List of available command mode options
 *
 * Defines a list of available options of a tool command mode. Option list
 * entries are defined with OPT_ENTRY_*() macros.
 *
 * A help option that shows the the help menu is added to the command mode
 * option list by default.
 * A verbose option that increases verbosity is added to the command mode option
 * list by default. If set, debug logging will be printed.
 */
#define OPT_LIST(n, ...)						       \
	struct cmd_opt n[] = {						       \
		__VA_ARGS__,						       \
		OPT_ENTRY_FLAG("help", 'h', NULL, "Show help menu"),	       \
		OPT_ENTRY_INC("verbose", 'v', &util_log_level,		       \
			       "Increase verbosity"),			       \
		OPT_ENTRY_END()						       \
	}

/**
 * MODE_ENTRY:
 * @n: Name of the command mode
 * @h: Short description of the command mode
 * @f: Pointer to the command mode handler
 * @dev_req: Whether a device argument is required for the command mode
 *
 * Defines a mode entry in the mode list of a tool command.
 */
#define MODE_ENTRY(n, h, f, dev_req)					       \
	{								       \
		.name = n,						       \
		.help = h,						       \
		.func = f,						       \
		.device_req = dev_req,					       \
	}

/**
 * MODE_ENTRY_DEFAULT:
 * @h: Short description of the command mode
 * @f: Pointer to the command mode handler
 * @dev_req: Whether a device argument is required for the command mode
 *
 * Defines a default mode entry in the mode list of a tool command. The default
 * mode will be used in case no mode is specified in the command line arguments.
 * Only one default mode is allowed per command.
 */
#define MODE_ENTRY_DEFAULT(h, f, dev_req)				       \
	{								       \
		.name = "default",					       \
		.help = h,						       \
		.func = f,						       \
		.device_req = dev_req,					       \
		.default_mode = true,					       \
	}

#define MODE_ENTRY_END() { 0 }

/**
 * MODE_LIST:
 * @n: Name of the variable that holds the command mode list
 * @...: List of available command modes
 *
 * Defines a list of available modes of a tool command. Mode list entries are
 * defined with MODE_ENTRY().
 */
#define MODE_LIST(n, ...)						       \
	struct cmd_mode n[] = {						       \
		__VA_ARGS__,						       \
		MODE_ENTRY_END()					       \
	}

/**
 * CMD_ENTRY:
 * @n: Name of the command
 * @h: Short description of the command
 * @f: Pointer to the command handler
 *
 * Defines a command entry in the command list of a tool.
 */
#define CMD_ENTRY(n, h, f)						       \
	{								       \
		.name = n,						       \
		.help = h,						       \
		.func = f,						       \
	}

#define CMD_ENTRY_END() { 0 }

/**
 * CMD_LIST:
 * @n: Name of the variable that holds the command list
 * @...: List of available commands
 *
 * Defines a list of available commands of a tool. Command list entries are
 * defined with CMD_ENTRY().
 *
 * A version command that shows the tool version is added to the command list
 * by default.
 */
#define CMD_LIST(n, ...)						       \
	struct cmd n[] = {						       \
		__VA_ARGS__,						       \
		CMD_ENTRY_END()						       \
	}

/**
 * cmd_parse:
 * @argc: Number of command line arguments
 * @argv: Command line arguments
 * @help: Short description of the tool
 * @cmds: List of available commands
 *
 * Parses the command portion of the command line arguments.
 *
 * Returns 0 on success, or an appropriate errno error code on failure.
 */
int cmd_parse(int argc, char *argv[], const char *help, struct cmd *cmds);

/**
 * cmd_mode_parse:
 * @argc: Number of command line arguments
 * @argv: Command line arguments
 * @cmd: Command that was parsed
 * @modes: List of available modes of the command
 *
 * Parses the mode portion of the command line arguments.
 *
 * Returns 0 on success, or an appropriate errno error code on failure.
 */
int cmd_mode_parse(int argc, char *argv[], struct cmd *cmd,
		   struct cmd_mode *modes);

/**
 * cmd_opt_parse:
 * @argc: Number of command line arguments
 * @argv: Command line arguments
 * @mode: Mode that was parsed
 * @device: Pointer to a string that will be set to the device name
 * @opts: List of available options of the command mode
 *
 * Parses the device and option portions of the command line arguments.
 *
 * Returns 0 on success, or an appropriate errno error code on failure.
 */
int cmd_opt_parse(int argc, char *argv[], struct cmd_mode *mode, char **device,
		  struct cmd_opt *opts);

#endif
