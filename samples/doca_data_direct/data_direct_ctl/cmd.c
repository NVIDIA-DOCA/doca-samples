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

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"

#define CMD_IDX 1
#define MODE_IDX 2
#define DEV_IDX 3
#define CMD_ARGC 2
#define MODE_ARGC 3
#define DEV_ARGC 4

static int cmd_opt_get_arg(struct cmd_opt *opt)
{
	switch (opt->type) {
	case CMD_OPT_TYPE_FLAG:
		*((bool *)opt->value) = true;

		break;
	case CMD_OPT_TYPE_INC:
		*((int *)opt->value) += 1;

		break;
	case CMD_OPT_TYPE_BOOL:
		if (!strcmp(optarg, "1") || !strcmp(optarg, "true")) {
			*((bool *)opt->value) = true;
		} else if (!strcmp(optarg, "0") || !strcmp(optarg, "false")) {
			*((bool *)opt->value) = false;
		} else {
			fprintf(stderr,
				"Error: Invalid argument for a boolean option\n");
			return EINVAL;
		}

		break;
	default:
		fprintf(stderr, "Error: Unknown command option type %d\n",
			opt->type);
		return EINVAL;
	}

	opt->is_set = true;

	return 0;
}

static int cmd_opt_find_opt(struct cmd_opt *opts, int opt)
{
	for (; opts->long_opt; opts++)
		if (opts->short_opt == opt)
			return cmd_opt_get_arg(opts);

	fprintf(stderr, "Error: Couldn't find option '%c'\n", opt);

	return EINVAL;
}

static int cmd_opt_parse_device(int argc, char *argv[], struct cmd_mode *mode,
				char **device)
{
	int dev_argc = DEV_ARGC;
	int dev_idx = DEV_IDX;

	if (mode->default_mode) {
		dev_argc--;
		dev_idx--;
	}

	switch (mode->device_req) {
	case CMD_MODE_DEVICE_REQ_YES:
		assert(device);

		if (argc < dev_argc || argv[dev_idx][0] == '-') {
			fprintf(stderr,
				"Error: Device was not found but is required\n");
			return EINVAL;
		}

		*device = argv[dev_idx];

		break;
	case CMD_MODE_DEVICE_REQ_OPTIONAL:
		assert(device);

		if (argc >= dev_argc && argv[dev_idx][0] != '-')
			*device = argv[dev_idx];
		break;
	case CMD_MODE_DEVICE_REQ_NO:
		break;
	default:
		assert(false);
		fprintf(stderr,
			"Error: Invalid device requirement for command mode %s\n",
			mode->name);
		return EINVAL;
	}

	return 0;
}

static void cmd_opt_help(const char *prog_name, const char *cmd_name,
			 struct cmd_mode *mode, struct cmd_opt *opts)
{
	const char *device;

	switch (mode->device_req) {
	case CMD_MODE_DEVICE_REQ_YES:
		device = " <device>";
		break;
	case CMD_MODE_DEVICE_REQ_OPTIONAL:
		device = " [device]";
		break;
	case CMD_MODE_DEVICE_REQ_NO:
	default:
		device = "";
		break;
	}

	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s %s%s%s%s [OPTIONS]\n\n", prog_name, cmd_name,
		mode->default_mode ? "" : " ",
		mode->default_mode ? "" : mode->name, device);
	fprintf(stderr, "%s\n\n", mode->help);
	fprintf(stderr, "Options:\n");

	for (; opts->long_opt; opts++) {
		fprintf(stderr, "  [ -%c ", opts->short_opt);
		if (opts->has_arg) {
			if (opts->required)
				fprintf(stderr, "<%s> ", opts->type_help);
			else
				fprintf(stderr, "[<%s>] ", opts->type_help);
		}

		fprintf(stderr, "| --%s", opts->long_opt);
		if (opts->has_arg) {
			if (opts->required)
				fprintf(stderr, "=<%s>", opts->type_help);
			else
				fprintf(stderr, "=[<%s>]", opts->type_help);
		}

		fprintf(stderr, "] -- %s\n", opts->help);
	}
}

#define SHORT_OPT_MAX_LEN 3
#define CMD_NAME_MAX_SIZE 64
int cmd_opt_parse(int argc, char *argv[], struct cmd_mode *mode, char **device,
		  struct cmd_opt *opts)
{
	struct option *long_options;
	struct option *cur_long_opt;
	struct cmd_opt *cur_opt;
	char *short_options;
	int num_opts = 0;
	char *cmd_name;
	int pos_args;
	int index;
	int ret;
	int c;

	/* getopt_long() permutates argv, store cmd name to show in help menu */
	cmd_name = argv[CMD_IDX];

	for (cur_opt = opts; cur_opt->long_opt; cur_opt++)
		num_opts++;

	long_options = calloc(num_opts, sizeof(*long_options));
	if (!long_options) {
		fprintf(stderr, "Error: Failed to allcoate long options\n");
		return ENOMEM;
	}

	/* +1 for ':' char in the beginning */
	short_options = calloc(1, (num_opts * SHORT_OPT_MAX_LEN) + 1);
	if (!short_options) {
		fprintf(stderr, "Error: Failed to allocate short options\n");
		free(long_options);

		return ENOMEM;
	}

	ret = cmd_opt_parse_device(argc, argv, mode, device);
	if (ret)
		goto err;

	index = 0;
	short_options[index++] = ':';
	for (cur_opt = opts, cur_long_opt = long_options; cur_opt->long_opt;
	     cur_opt++, cur_long_opt++) {
		cur_long_opt->name = cur_opt->long_opt;
		cur_long_opt->has_arg = cur_opt->has_arg;
		cur_long_opt->val = cur_opt->short_opt;

		short_options[index++] = cur_opt->short_opt;
		if (cur_opt->has_arg == required_argument) {
			short_options[index++] = ':';
		} else if (cur_opt->has_arg == optional_argument) {
			short_options[index++] = ':';
			short_options[index++] = ':';
		}
	}

	while ((c = getopt_long(argc, argv, short_options, long_options,
				NULL)) != -1) {
		switch (c) {
		case '?':
			fprintf(stderr, "Error: Unknown option '%s'\n",
				argv[optind - 1]);
			ret = EINVAL;
			goto err;
		case ':':
			fprintf(stderr,
				"Error: Option '%s' requires an argument\n",
				argv[optind - 1]);
			ret = EINVAL;
			goto err;
		case 'h':
			ret = EINVAL;
			goto err;
		default:
			ret = cmd_opt_find_opt(opts, c);
			if (ret)
				goto err;

			break;
		}
	}

	for (cur_opt = opts; cur_opt->long_opt; cur_opt++) {
		if (cur_opt->required && !cur_opt->is_set) {
			fprintf(stderr,
				"Error: Required option [ -%c | --%s ] was not set\n",
				cur_opt->short_opt, cur_opt->long_opt);
			ret = EINVAL;
			goto err;
		}
	}

	pos_args = device ? 3 : 2;
	if (mode->default_mode)
		pos_args--;
	if (optind < argc - pos_args) {
		fprintf(stderr, "Error: Extraneous positional arguments\n");
		ret = EINVAL;
		goto err;
	}

	free(short_options);
	free(long_options);

	return 0;

err:
	cmd_opt_help(argv[0], cmd_name, mode, opts);

	free(short_options);
	free(long_options);

	return ret;
}

bool cmd_opt_is_set(struct cmd_opt *opts, char short_opt)
{
	for (; opts->long_opt; opts++)
		if (opts->short_opt == short_opt)
			return opts->is_set;

	assert(false);
	return false;
}

static struct cmd_mode *cmd_mode_get_default(struct cmd_mode *modes)
{
	for (; modes->name; modes++)
		if (modes->default_mode)
			return modes;

	return NULL;
}

static void cmd_mode_help(int argc, char *argv[], struct cmd *cmd,
			  struct cmd_mode *modes)
{
	struct cmd_mode *default_mode;
	char *mode_str;

	default_mode = cmd_mode_get_default(modes);
	mode_str = default_mode ? "[mode]" : "<mode>";

	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s %s %s [device] [OPTIONS]\n\n", argv[0],
		argv[CMD_IDX], mode_str);
	fprintf(stderr, "%s\n\n", cmd->help);

	fprintf(stderr, "Modes:\n");
	if (default_mode)
		fprintf(stderr, "  Default (no mode specified) --- %s\n",
			default_mode->help);

	for (; modes->name; modes++) {
		if (modes->default_mode)
			continue;
		fprintf(stderr, "  %s --- %s\n", modes->name, modes->help);
	}
	fprintf(stderr, "  help --- Show help menu\n");
}

int cmd_mode_parse(int argc, char *argv[], struct cmd *cmd,
		   struct cmd_mode *modes)
{
	struct cmd_mode *default_mode;
	struct cmd_mode *mode;
	int ret;

	default_mode = cmd_mode_get_default(modes);

	if (argc < MODE_ARGC) {
		if (default_mode) {
			if (default_mode->device_req !=
			    CMD_MODE_DEVICE_REQ_YES)
				return default_mode->func(argc, argv,
							  default_mode);

			fprintf(stderr, "Error: Device must be supplied\n");
			ret = EINVAL;
			goto out;
		}
		fprintf(stderr, "Error: Command mode must be supplied\n");
		ret = EINVAL;
		goto out;
	}

	for (mode = modes; mode->name; mode++)
		if (!strcmp(argv[MODE_IDX], mode->name))
			return mode->func(argc, argv, mode);

	if (!strcmp(argv[MODE_IDX], "help")) {
		ret = 0;
		goto out;
	}

	if (default_mode)
		if (argv[MODE_IDX][0] == '-' ||
		    !strncmp("pci/", argv[MODE_IDX], 4))
			return default_mode->func(argc, argv, default_mode);

	fprintf(stderr, "Error: Unknown command mode '%s'\n", argv[MODE_IDX]);
	ret = EINVAL;

out:
	cmd_mode_help(argc, argv, cmd, modes);

	return ret;
}

static void cmd_help(int argc, char *argv[], const char *help, struct cmd *cmds)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s <command> <mode> [device] [OPTIONS]\n\n",
		argv[0]);
	fprintf(stderr, "%s\n\n", help);
	fprintf(stderr, "Commands:\n");

	for (; cmds->name; cmds++)
		fprintf(stderr, "  %s --- %s\n", cmds->name, cmds->help);

	fprintf(stderr, "  help --- Show help menu\n");
}

int cmd_parse(int argc, char *argv[], const char *help, struct cmd *cmds)
{
	struct cmd *cmd;
	int ret;

	if (argc < CMD_ARGC) {
		fprintf(stderr, "Error: Command name must be supplied\n");
		ret = EINVAL;
		goto out;
	}

	for (cmd = cmds; cmd->name; cmd++)
		if (!strcmp(argv[CMD_IDX], cmd->name))
			return cmd->func(argc, argv, cmd);

	if (!strcmp(argv[CMD_IDX], "help")) {
		ret = 0;
		goto out;
	}

	fprintf(stderr, "Error: Unknown command '%s'\n", argv[CMD_IDX]);
	ret = EINVAL;

out:
	cmd_help(argc, argv, help, cmds);

	return ret;
}
