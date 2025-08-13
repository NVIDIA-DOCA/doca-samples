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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "data_direct.h"

#include "ccan/json/json.h"

#include "cmd.h"

static void add_json_entry(JsonNode *root, const char *device,
			   const char *data_direct_device)
{
	JsonNode *data_direct_device_str;
	JsonNode *device_obj;

	data_direct_device_str = json_mkstring(data_direct_device);
	device_obj = json_mkobject();
	json_append_member(device_obj, "data-direct-device",
			   data_direct_device_str);

	json_append_member(root, device, device_obj);
}

static void show_json(JsonNode *root)
{
	char *json_str;

	json_str = json_stringify(root, JSON_SPACE);
	printf("%s\n", json_str);
	free(json_str);
}

static void show_json_data_direct_device(const char *device,
					 const char *data_direct_device)
{
	JsonNode *root;

	root = json_mkobject();
	add_json_entry(root, device, data_direct_device);

	show_json(root);
	json_delete(root);
}

static void show_json_data_direct_devices(struct dd_array *dd_arr)
{
	JsonNode *root;
	int i;

	root = json_mkobject();
	for (i = 0; i < dd_arr->len; i++)
		add_json_entry(root, dd_arr->entries[i].device,
			       dd_arr->entries[i].data_direct_device);

	show_json(root);
	json_delete(root);
}

static void show_entry(const char *device, const char *data_direct_device)
{
	printf("%s:\n", device);
	printf("  data-direct-device: %s\n", data_direct_device);
}

static void show_data_direct_device(const char *device,
				    const char *data_direct_device, bool json)
{
	if (json) {
		show_json_data_direct_device(device, data_direct_device);
		return;
	}

	show_entry(device, data_direct_device);
}

static void show_data_direct_devices(struct dd_array *dd_arr, bool json)
{
	int i;

	if (json) {
		show_json_data_direct_devices(dd_arr);
		return;
	}

	for (i = 0; i < dd_arr->len; i++)
		show_entry(dd_arr->entries[i].device,
			   dd_arr->entries[i].data_direct_device);
}

static int cmd_show_data_direct_device(int argc, char *argv[],
				       struct cmd_mode *mode)
{
	char *data_direct_device;
	struct dd_array *dd_arr;
	char *device = NULL;
	int ret;

	struct cmd_options {
		bool json;
	} cmd_opts = {};

	OPT_LIST(opts,
		 OPT_ENTRY_JSON(&cmd_opts.json));

	ret = cmd_opt_parse(argc, argv, mode, &device, opts);
	if (ret)
		return ret;

	if (!device) {
		dd_arr = dd_get_all_data_direct_devices();
		if (!dd_arr)
			return errno;

		show_data_direct_devices(dd_arr, cmd_opts.json);
		dd_array_free(dd_arr);

		return 0;
	}

	data_direct_device = dd_get_data_direct_device(device);
	if (!data_direct_device) {
		if (errno == EOPNOTSUPP)
			log_err("Data direct VUID query is not supported for %s",
				device);

		return errno;
	}

	show_data_direct_device(device, data_direct_device, cmd_opts.json);

	free(data_direct_device);

	return 0;
}

static int cmd_show_data_direct(int argc, char *argv[], struct cmd_mode *mode)
{
	char *device;
	bool state;
	int ret;

	struct cmd_options {
		bool json;
	} cmd_opts = {};

	OPT_LIST(opts,
		 OPT_ENTRY_JSON(&cmd_opts.json));

	ret = cmd_opt_parse(argc, argv, mode, &device, opts);
	if (ret)
		return ret;

	ret = dd_get_data_direct_state(device, &state);
	if (ret)
		return ret;

	if (cmd_opts.json) {
		JsonNode *data_direct_str;
		JsonNode *device_obj;
		char *json_str;
		JsonNode *root;

		root = json_mkobject();
		data_direct_str = json_mkstring(state ? "enabled" : "disabled");
		device_obj = json_mkobject();
		json_append_member(device_obj, "data-direct", data_direct_str);
		json_append_member(root, device, device_obj);

		json_str = json_stringify(root, JSON_SPACE);
		printf("%s\n", json_str);

		free(json_str);
		json_delete(root);
	} else {
		printf("%s:\n", device);
		printf("  data-direct: %s\n", state ? "enabled" : "disabled");
	}

	return 0;
}

static int cmd_show(int argc, char *argv[], struct cmd *cmd)
{
	MODE_LIST(modes,
	          MODE_ENTRY("data-direct",
			     "Show the data direct state of the device",
			     cmd_show_data_direct, CMD_MODE_DEVICE_REQ_YES),
		  MODE_ENTRY("data-direct-device",
			     "Show the mapping of devices and their data direct devices",
			     cmd_show_data_direct_device,
			     CMD_MODE_DEVICE_REQ_OPTIONAL));

	return cmd_mode_parse(argc, argv, cmd, modes);
}

static int cmd_set_data_direct(int argc, char *argv[], struct cmd_mode *mode)
{
	char *device;
	int ret;

	struct cmd_options {
		bool enable;
	} cmd_opts = {};

	OPT_LIST(opts,
		 OPT_ENTRY_BOOL("enable", 'e', &cmd_opts.enable,
				"Enable or disable data direct", true));

	ret = cmd_opt_parse(argc, argv, mode, &device, opts);
	if (ret)
		return ret;

	return dd_set_data_direct_state(device, cmd_opts.enable);
}

static int cmd_set(int argc, char *argv[], struct cmd *cmd)
{
	MODE_LIST(modes,
		  MODE_ENTRY("data-direct",
			     "Enable or disable data direct",
			     cmd_set_data_direct, CMD_MODE_DEVICE_REQ_YES));

	return cmd_mode_parse(argc, argv, cmd, modes);
}

int main(int argc, char *argv[])
{
	const char *help = "Manage data direct configuration";

	CMD_LIST(cmds,
		 CMD_ENTRY("set", "Set data direct configuration", cmd_set),
		 CMD_ENTRY("show", "Shows data direct information", cmd_show));

	return cmd_parse(argc, argv, help, cmds);
}
