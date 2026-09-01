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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_error.h>
#include <doca_mgmt.h>

#include "mgmt_common.h"

/* Number of spaces of a single indentation level in the target listing */
#define MGMT_COMMON_INDENT_UNIT 4
/* Width the attribute label is padded to in verbose listing, so that the values are aligned in a column */
#define MGMT_COMMON_ATTR_LABEL_WIDTH 30
/* Token printed for an attribute that is not available (not supported) for a given target */
#define MGMT_COMMON_NOT_AVAILABLE "N/A"

/*
 * Whether the "list-targets" command should print all attributes of each target. Set by the "--verbose" flag callback
 * and read by mgmt_common_list_targets(). Since the samples are single-shot CLI tools, keeping this state private to
 * the common module keeps every sample decoupled from the flag.
 */
static bool g_list_targets_verbose;

/*
 * Get a human-readable string for a PCI function type.
 *
 * @pci_func_type [in]: the PCI function type
 * @return: a string representation of the PCI function type
 */
static const char *pci_func_type_to_str(enum doca_pci_func_type pci_func_type)
{
	switch (pci_func_type) {
	case DOCA_PCI_FUNC_TYPE_PF:
		return "PF";
	case DOCA_PCI_FUNC_TYPE_VF:
		return "VF";
	case DOCA_PCI_FUNC_TYPE_SF:
		return "SF";
	default:
		return "UNKNOWN";
	}
}

/*
 * Get a human-readable string for a representor PCI PF type.
 *
 * @pci_pf_type [in]: the PCI PF type
 * @return: a string representation of the PCI PF type
 */
static const char *mgmt_pci_pf_type_to_str(enum doca_mgmt_pci_pf_type pci_pf_type)
{
	switch (pci_pf_type) {
	case DOCA_MGMT_PCI_PF_TYPE_EXTERNAL_HOST:
		return "external-host";
	case DOCA_MGMT_PCI_PF_TYPE_SATELLITE:
		return "satellite";
	default:
		return "UNKNOWN";
	}
}

/*
 * Print a bare line (e.g., a target string) at the given indentation level.
 *
 * @indent_level [in]: indentation level
 * @line [in]: the line to print
 */
static void print_line(int indent_level, const char *line)
{
	printf("%*s%s\n", indent_level * MGMT_COMMON_INDENT_UNIT, "", line);
}

/*
 * Print a "label value" attribute row at the given indentation level, with the label padded to a fixed width.
 *
 * @indent_level [in]: indentation level
 * @label [in]: the attribute label
 * @value [in]: the attribute value
 */
static void print_attr_str(int indent_level, const char *label, const char *value)
{
	printf("%*s%-*s%s\n", indent_level * MGMT_COMMON_INDENT_UNIT, "", MGMT_COMMON_ATTR_LABEL_WIDTH, label, value);
}

/*
 * Print an unsigned integer attribute row at the given indentation level.
 *
 * @indent_level [in]: indentation level
 * @label [in]: the attribute label
 * @value [in]: the attribute value
 */
static void print_attr_uint(int indent_level, const char *label, unsigned int value)
{
	printf("%*s%-*s%u\n", indent_level * MGMT_COMMON_INDENT_UNIT, "", MGMT_COMMON_ATTR_LABEL_WIDTH, label, value);
}

/*
 * Print the attributes of a device management target (verbose listing).
 *
 * @info [in]: the device target info entry
 */
static void print_dev_attrs(const struct doca_mgmt_target_info_dev *info)
{
	enum doca_pci_func_type pci_func_type;
	uint8_t native_port_num;
	const char *str;
	doca_error_t result;

	result = doca_mgmt_target_info_dev_get_pci_func_type(info, &pci_func_type);
	print_attr_str(1,
		       "pci_func_type",
		       result == DOCA_SUCCESS ? pci_func_type_to_str(pci_func_type) : MGMT_COMMON_NOT_AVAILABLE);

	result = doca_mgmt_target_info_dev_get_native_port_num(info, &native_port_num);
	if (result == DOCA_SUCCESS)
		print_attr_uint(1, "native_port_num", native_port_num);
	else
		print_attr_str(1, "native_port_num", MGMT_COMMON_NOT_AVAILABLE);

	result = doca_mgmt_target_info_dev_get_iface_name(info, &str);
	print_attr_str(1, "iface_name", result == DOCA_SUCCESS ? str : MGMT_COMMON_NOT_AVAILABLE);

	result = doca_mgmt_target_info_dev_get_ibdev_name(info, &str);
	print_attr_str(1, "ibdev_name", result == DOCA_SUCCESS ? str : MGMT_COMMON_NOT_AVAILABLE);
}

/*
 * Print the attributes of a representor management target (verbose listing).
 *
 * @rep_info [in]: the representor target info entry
 */
static void print_rep_attrs(const struct doca_mgmt_target_info_dev_rep *rep_info)
{
	enum doca_pci_func_type pci_func_type;
	enum doca_mgmt_pci_pf_type pci_pf_type;
	uint32_t value;
	uint16_t vhca_id;
	const char *str;
	bool have_func_type;
	doca_error_t result;

	result = doca_mgmt_target_info_dev_rep_get_pci_func_type(rep_info, &pci_func_type);
	have_func_type = (result == DOCA_SUCCESS);
	print_attr_str(2,
		       "pci_func_type",
		       have_func_type ? pci_func_type_to_str(pci_func_type) : MGMT_COMMON_NOT_AVAILABLE);

	/* PCI PF type is only relevant for PF representors */
	if (have_func_type && pci_func_type == DOCA_PCI_FUNC_TYPE_PF) {
		result = doca_mgmt_target_info_dev_rep_get_pci_pf_type(rep_info, &pci_pf_type);
		print_attr_str(2,
			       "pci_pf_type",
			       result == DOCA_SUCCESS ? mgmt_pci_pf_type_to_str(pci_pf_type) :
							MGMT_COMMON_NOT_AVAILABLE);
	}

	result = doca_mgmt_target_info_dev_rep_get_controller_num(rep_info, &value);
	if (result == DOCA_SUCCESS)
		print_attr_uint(2, "controller_num", value);
	else
		print_attr_str(2, "controller_num", MGMT_COMMON_NOT_AVAILABLE);

	result = doca_mgmt_target_info_dev_rep_get_pf_index(rep_info, &value);
	if (result == DOCA_SUCCESS)
		print_attr_uint(2, "pf_index", value);
	else
		print_attr_str(2, "pf_index", MGMT_COMMON_NOT_AVAILABLE);

	/* VF/SF index are valid only for the matching representor type, so query only the relevant one */
	if (have_func_type && pci_func_type == DOCA_PCI_FUNC_TYPE_VF) {
		result = doca_mgmt_target_info_dev_rep_get_vf_index(rep_info, &value);
		if (result == DOCA_SUCCESS)
			print_attr_uint(2, "vf_index", value);
	} else if (have_func_type && pci_func_type == DOCA_PCI_FUNC_TYPE_SF) {
		result = doca_mgmt_target_info_dev_rep_get_sf_index(rep_info, &value);
		if (result == DOCA_SUCCESS)
			print_attr_uint(2, "sf_index", value);
	}

	result = doca_mgmt_target_info_dev_rep_get_vhca_id(rep_info, &vhca_id);
	if (result == DOCA_SUCCESS)
		print_attr_uint(2, "vhca_id", vhca_id);
	else
		print_attr_str(2, "vhca_id", MGMT_COMMON_NOT_AVAILABLE);

	result = doca_mgmt_target_info_dev_rep_get_iface_name(rep_info, &str);
	print_attr_str(2, "iface_name", result == DOCA_SUCCESS ? str : MGMT_COMMON_NOT_AVAILABLE);
}

/*
 * Print the representor management targets of a device, nested beneath it.
 * Best-effort: a failure to enumerate the representors is reported inline and does not abort the listing.
 *
 * @dev_info [in]: the device target info entry whose representors to print
 */
static void print_dev_reps(const struct doca_mgmt_target_info_dev *dev_info)
{
	struct doca_mgmt_target_info_dev_rep **rep_list = NULL;
	uint32_t rep_count = 0;
	uint32_t i;
	doca_error_t result;

	result = doca_mgmt_target_info_dev_rep_create_list(dev_info, &rep_list, &rep_count);
	if (result != DOCA_SUCCESS) {
		char line[128];

		snprintf(line, sizeof(line), "(representors unavailable: %s)", doca_error_get_descr(result));
		print_line(1, line);
		return;
	}

	for (i = 0; i < rep_count; i++) {
		const char *target_string = NULL;

		(void)doca_mgmt_target_info_dev_rep_get_target_string(rep_list[i], &target_string);
		print_line(1, target_string != NULL ? target_string : MGMT_COMMON_NOT_AVAILABLE);

		if (g_list_targets_verbose)
			print_rep_attrs(rep_list[i]);
	}

	result = doca_mgmt_target_info_dev_rep_destroy_list(rep_list);
	if (result != DOCA_SUCCESS)
		fprintf(stderr,
			"Failed to destroy representor management targets list: %s\n",
			doca_error_get_descr(result));
}

doca_error_t mgmt_common_list_targets(bool include_reps)
{
	struct doca_mgmt_target_info_dev **dev_list = NULL;
	uint32_t dev_count = 0;
	uint32_t i;
	doca_error_t result;

	result = doca_mgmt_target_info_dev_create_list(&dev_list, &dev_count);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to list device management targets: %s\n", doca_error_get_descr(result));
		return result;
	}

	if (dev_count == 0) {
		printf("No management targets found.\n");
		goto destroy_dev_list;
	}

	for (i = 0; i < dev_count; i++) {
		const char *target_string = NULL;

		(void)doca_mgmt_target_info_dev_get_target_string(dev_list[i], &target_string);
		print_line(0, target_string != NULL ? target_string : MGMT_COMMON_NOT_AVAILABLE);

		if (g_list_targets_verbose)
			print_dev_attrs(dev_list[i]);

		if (include_reps)
			print_dev_reps(dev_list[i]);
	}

destroy_dev_list:
	result = doca_mgmt_target_info_dev_destroy_list(dev_list);
	if (result != DOCA_SUCCESS)
		fprintf(stderr, "Failed to destroy device management targets list: %s\n", doca_error_get_descr(result));

	return result;
}

/*
 * ARGP Callback - Handle the "list-targets" verbose flag
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t list_targets_verbose_callback(void *param, void *config)
{
	(void)config;

	g_list_targets_verbose = *(bool *)param;

	return DOCA_SUCCESS;
}

doca_error_t mgmt_common_register_list_targets_cmd(doca_argp_cmd_cb_t callback)
{
	struct doca_argp_cmd *cmd;
	struct doca_argp_param *verbose_param;
	doca_error_t result;

	/* Create the list-targets command */
	result = doca_argp_cmd_create(&cmd);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create ARGP command: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_cmd_set_name(cmd, "list-targets");
	doca_argp_cmd_set_description(cmd, "List the available management targets");
	doca_argp_cmd_set_callback(cmd, callback);

	/* Create and register the verbose flag */
	result = doca_argp_param_create(&verbose_param);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		(void)doca_argp_cmd_destroy(cmd);
		return result;
	}
	doca_argp_param_set_short_name(verbose_param, "v");
	doca_argp_param_set_long_name(verbose_param, "verbose");
	doca_argp_param_set_description(verbose_param,
					"Print all attributes of each target, not only the target string");
	doca_argp_param_set_callback(verbose_param, list_targets_verbose_callback);
	doca_argp_param_set_type(verbose_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_cmd_register_param(cmd, verbose_param);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register verbose param: %s\n", doca_error_get_descr(result));
		(void)doca_argp_cmd_destroy(cmd);
		return result;
	}

	/* Register the list-targets command */
	result = doca_argp_register_cmd(cmd);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to register list-targets command: %s\n", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create and register a management target string parameter for a command.
 *
 * @cmd [in]: the command to register the parameter for
 * @short_name [in]: the parameter's short name
 * @long_name [in]: the parameter's long name
 * @description [in]: the parameter's description
 * @callback [in]: the parameter's callback
 * @mandatory [in]: whether the parameter is mandatory
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_target_param(struct doca_argp_cmd *cmd,
					  const char *short_name,
					  const char *long_name,
					  const char *description,
					  doca_argp_param_cb_t callback,
					  bool mandatory)
{
	struct doca_argp_param *param;
	doca_error_t result;

	result = doca_argp_param_create(&param);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "Failed to create ARGP param: %s\n", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(param, short_name);
	doca_argp_param_set_long_name(param, long_name);
	doca_argp_param_set_description(param, description);
	doca_argp_param_set_callback(param, callback);
	doca_argp_param_set_type(param, DOCA_ARGP_TYPE_STRING);
	if (mandatory)
		doca_argp_param_set_mandatory(param);

	result = doca_argp_cmd_register_param(cmd, param);
	if (result != DOCA_SUCCESS)
		fprintf(stderr, "Failed to register target param: %s\n", doca_error_get_descr(result));

	return result;
}

doca_error_t mgmt_common_register_target_dev_param(struct doca_argp_cmd *cmd,
						   doca_argp_param_cb_t callback,
						   bool mandatory,
						   const char *description)
{
	if (description == NULL)
		description =
			"DOCA device management target string (e.g., pci/0000:08:00.0). Use 'list-targets' to discover available targets";

	return register_target_param(cmd, "d", "device", description, callback, mandatory);
}

doca_error_t mgmt_common_register_target_dev_rep_param(struct doca_argp_cmd *cmd,
						       doca_argp_param_cb_t callback,
						       bool mandatory,
						       const char *description)
{
	if (description == NULL)
		description =
			"DOCA device representor management target string (e.g., pci/0000:08:00.0,c0pf0vf0). Use 'list-targets' to discover available targets";

	return register_target_param(cmd, "r", "rep", description, callback, mandatory);
}

doca_error_t mgmt_common_target_string_copy(const char *target_string, char *dst)
{
	size_t len = strnlen(target_string, MGMT_COMMON_TARGET_STR_LEN);

	if (len >= MGMT_COMMON_TARGET_STR_LEN) {
		fprintf(stderr, "Target string exceeds the maximum size of %d\n", MGMT_COMMON_TARGET_STR_LEN - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	strncpy(dst, target_string, len + 1);

	return DOCA_SUCCESS;
}
