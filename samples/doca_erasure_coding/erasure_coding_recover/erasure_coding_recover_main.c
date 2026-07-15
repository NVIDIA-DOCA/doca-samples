/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <stdlib.h>
#include <string.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_erasure_coding.h>
#include <doca_error.h>
#include <doca_log.h>

#include <utils.h>

DOCA_LOG_REGISTER(EC_RECOVER::MAIN);

#define USER_MAX_PATH_NAME 255		       /* max file name length */
#define MAX_PATH_NAME (USER_MAX_PATH_NAME + 1) /* max file name string length */
#define MAX_BLOCKS (128 + 32)		       /* ec blocks up to 128 in, 32 out */

/* Configuration struct */
struct ec_cfg {
	char input_path[MAX_PATH_NAME];	 /* input file to encode or input blocks dir to decode */
	char output_path[MAX_PATH_NAME]; /* output might be a file or a folder - depends on the input and do_both */
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE]; /* device PCI address */
	bool do_both;				      /* to do full process - encoding & decoding */
	enum doca_ec_matrix_type matrix;	      /* ec matrix type */
	uint32_t data_block_count;		      /* data block count */
	uint32_t rdnc_block_count;		      /* redundancy block count */
	size_t n_delete_block;			      /* number of deleted block indices */
	uint32_t delete_block_indices[MAX_BLOCKS];    /* indices of data blocks to delete */
	int num_src_buf;			      /* Number of linked_list doca_buf element for the source buffer */
	int num_dst_buf; /* Number of linked_list doca_buf element for the destination buffer */
};

/* Sample's Logic */
doca_error_t ec_recover(const char *pci_addr,
			const char *input_path,
			const char *output_path,
			bool do_both,
			enum doca_ec_matrix_type matrix_type,
			uint32_t data_block_count,
			uint32_t rdnc_block_count,
			uint32_t *missing_indices,
			size_t n_missing,
			int num_src_buf,
			int num_dst_buf);

/*
 * ARGP Callback - Handle PCI device address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t pci_address_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;
	char *pci_address = (char *)param;
	int len;

	len = strnlen(pci_address, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len >= DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(ec_cfg->pci_address, pci_address, len + 1);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle user input path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t input_path_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;
	char *path = (char *)param;
	int len;

	len = strnlen(path, MAX_PATH_NAME);
	if (len >= MAX_PATH_NAME) {
		DOCA_LOG_ERR("Invalid input path name length, max %d", USER_MAX_PATH_NAME);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (access(path, F_OK) == -1) {
		DOCA_LOG_ERR("Input file/folder not found %s", ec_cfg->input_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	strcpy(ec_cfg->input_path, path);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle user output path parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t output_path_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;
	char *file = (char *)param;
	int len;

	len = strnlen(file, MAX_PATH_NAME);
	if (len >= MAX_PATH_NAME) {
		DOCA_LOG_ERR("Invalid output path name length, max %d", USER_MAX_PATH_NAME);
		return DOCA_ERROR_INVALID_VALUE;
	}
	if (access(ec_cfg->output_path, F_OK) == -1) {
		DOCA_LOG_ERR("Output file/folder not found %s", ec_cfg->output_path);
		return DOCA_ERROR_NOT_FOUND;
	}
	strcpy(ec_cfg->output_path, file);
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle do both parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t do_both_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;

	ec_cfg->do_both = *(bool *)param;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle matrix parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t matrix_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;
	char *matrix = (char *)param;

	if (strcasecmp(matrix, "cauchy") == 0)
		ec_cfg->matrix = DOCA_EC_MATRIX_TYPE_CAUCHY;
	else if (strcasecmp(matrix, "vandermonde") == 0)
		ec_cfg->matrix = DOCA_EC_MATRIX_TYPE_VANDERMONDE;
	else {
		DOCA_LOG_ERR("Illegal mode = [%s]", matrix);
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle data block count parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t data_block_count_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;

	ec_cfg->data_block_count = *(uint32_t *)param;
	if (ec_cfg->data_block_count <= 0) {
		DOCA_LOG_ERR("Data block size should be bigger than 0");
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle redundancy block count parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t rdnc_block_count_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;

	ec_cfg->rdnc_block_count = *(uint32_t *)param;
	if (ec_cfg->rdnc_block_count <= 0) {
		DOCA_LOG_ERR("Redundancy block size should be bigger than 0");
		return DOCA_ERROR_INVALID_VALUE;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle deleted block indices parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t delete_block_indices_callback(void *param, void *config)
{
	struct ec_cfg *ec_cfg = (struct ec_cfg *)config;
	char *ind = (char *)param;
	int64_t num;

	ec_cfg->n_delete_block = 0;
	while (*ind != '\0') {
		num = strtol(ind, &ind, 10);
		if (num < 0) {
			DOCA_LOG_ERR("Delete block indices are negative");
			return DOCA_ERROR_INVALID_VALUE;
		}
		if (ec_cfg->n_delete_block + 1 > MAX_BLOCKS) {
			DOCA_LOG_ERR("Delete block indices count is bigger then max: %d, requested: %ld",
				     MAX_BLOCKS,
				     ec_cfg->n_delete_block);
			return DOCA_ERROR_INVALID_VALUE;
		}
		ec_cfg->delete_block_indices[ec_cfg->n_delete_block++] = num;
		while (*ind == ',')
			ind++;
	}
	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle number of source doca_buf parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_src_buf_callback(void *param, void *config)
{
	struct ec_cfg *conf = (struct ec_cfg *)config;
	const int *num = (int *)param;

	if (*num <= 0) {
		DOCA_LOG_ERR("Entered number of source doca_buf: %d, valid value must be >= 1", *num);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->num_src_buf = *num;

	return DOCA_SUCCESS;
}

/*
 * ARGP Callback - Handle number of destination doca_buf parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t num_dst_buf_callback(void *param, void *config)
{
	struct ec_cfg *conf = (struct ec_cfg *)config;
	const int *num = (int *)param;

	if (*num <= 0) {
		DOCA_LOG_ERR("Entered number of destination doca_buf: %d, valid value must be >= 1", *num);
		return DOCA_ERROR_INVALID_VALUE;
	}

	conf->num_dst_buf = *num;

	return DOCA_SUCCESS;
}

/*
 * Register the command line parameters for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_ec_params(void)
{
	doca_error_t result;
	struct doca_argp_param *pci_param, *input_path_param, *output_path_param, *do_both_param, *matrix_param,
		*data_block_count_param, *rdnc_block_count_param, *delete_block_indices_param, *num_src_buf_param,
		*num_dst_buf_param;

	result = doca_argp_param_create(&pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_param, "p");
	doca_argp_param_set_long_name(pci_param, "pci-addr");
	doca_argp_param_set_description(pci_param, "DOCA device PCI device address - default: 03:00.0");
	doca_argp_param_set_callback(pci_param, pci_address_callback);
	doca_argp_param_set_type(pci_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&input_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(input_path_param, "i");
	doca_argp_param_set_long_name(input_path_param, "input");
	doca_argp_param_set_description(input_path_param, "Input file/folder to ec - default: self");
	doca_argp_param_set_callback(input_path_param, input_path_callback);
	doca_argp_param_set_type(input_path_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(input_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&output_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(output_path_param, "o");
	doca_argp_param_set_long_name(output_path_param, "output");
	doca_argp_param_set_description(output_path_param, "Output file/folder to ec - default: /tmp");
	doca_argp_param_set_callback(output_path_param, output_path_callback);
	doca_argp_param_set_type(output_path_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(output_path_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&do_both_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(do_both_param, "b");
	doca_argp_param_set_long_name(do_both_param, "both");
	doca_argp_param_set_description(do_both_param, "Do both (encode & decode) - default: false");
	doca_argp_param_set_callback(do_both_param, do_both_callback);
	doca_argp_param_set_type(do_both_param, DOCA_ARGP_TYPE_BOOLEAN);
	result = doca_argp_register_param(do_both_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&matrix_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(matrix_param, "x");
	doca_argp_param_set_long_name(matrix_param, "matrix");
	doca_argp_param_set_description(matrix_param, "Matrix - {cauchy, vandermonde} - default: cauchy");
	doca_argp_param_set_callback(matrix_param, matrix_callback);
	doca_argp_param_set_type(matrix_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(matrix_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&data_block_count_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(data_block_count_param, "t");
	doca_argp_param_set_long_name(data_block_count_param, "data");
	doca_argp_param_set_description(data_block_count_param, "Data block count - default: 2");
	doca_argp_param_set_callback(data_block_count_param, data_block_count_callback);
	doca_argp_param_set_type(data_block_count_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(data_block_count_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&rdnc_block_count_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(rdnc_block_count_param, "r");
	doca_argp_param_set_long_name(rdnc_block_count_param, "rdnc");
	doca_argp_param_set_description(rdnc_block_count_param, "Redundancy block count - default: 2");
	doca_argp_param_set_callback(rdnc_block_count_param, rdnc_block_count_callback);
	doca_argp_param_set_type(rdnc_block_count_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(rdnc_block_count_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_argp_param_create(&delete_block_indices_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(delete_block_indices_param, "d");
	doca_argp_param_set_long_name(delete_block_indices_param, "delete-index");
	doca_argp_param_set_description(delete_block_indices_param,
					"Indices of data blocks to delete comma separated i.e. 0,3,4 - default: 0");
	doca_argp_param_set_callback(delete_block_indices_param, delete_block_indices_callback);
	doca_argp_param_set_type(delete_block_indices_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(delete_block_indices_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register number of source doca_buf param */
	result = doca_argp_param_create(&num_src_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(num_src_buf_param, "ns");
	doca_argp_param_set_long_name(num_src_buf_param, "num-src-buf");
	doca_argp_param_set_description(num_src_buf_param, "number of doca_buf for source buffer");
	doca_argp_param_set_callback(num_src_buf_param, num_src_buf_callback);
	doca_argp_param_set_type(num_src_buf_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(num_src_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create and register number of destination doca_buf param */
	result = doca_argp_param_create(&num_dst_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(num_dst_buf_param, "nd");
	doca_argp_param_set_long_name(num_dst_buf_param, "num-dst-buf");
	doca_argp_param_set_description(num_dst_buf_param, "number of doca_buf for destination buffer");
	doca_argp_param_set_callback(num_dst_buf_param, num_dst_buf_callback);
	doca_argp_param_set_type(num_dst_buf_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(num_dst_buf_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Sample main function
 *
 * @argc [in]: command line arguments size
 * @argv [in]: array of command line arguments
 * @return: EXIT_SUCCESS on success and EXIT_FAILURE otherwise
 */
int main(int argc, char **argv)
{
	doca_error_t result;
	int exit_status = EXIT_FAILURE;
	struct ec_cfg ec_cfg;
	struct doca_log_backend *sdk_log;
	int len;

	/* Register a logger backend */
	result = doca_log_backend_create_standard();
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	/* Register a logger backend for internal SDK errors and warnings */
	result = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
	if (result != DOCA_SUCCESS)
		goto sample_exit;
	result = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
	if (result != DOCA_SUCCESS)
		goto sample_exit;

	DOCA_LOG_INFO("Starting the sample");

	len = strnlen(argv[0], USER_MAX_PATH_NAME);
	if (len >= MAX_PATH_NAME) {
		DOCA_LOG_ERR("Self path is too long, max %d", USER_MAX_PATH_NAME);
		goto sample_exit;
	}
	strcpy(ec_cfg.pci_address, "03:00.0");
	strncpy(ec_cfg.input_path, argv[0], USER_MAX_PATH_NAME);
	strcpy(ec_cfg.output_path, "/tmp");
	ec_cfg.do_both = false; /* do both encoding & decoding (and delete data block between them) */
	ec_cfg.matrix = DOCA_EC_MATRIX_TYPE_CAUCHY;
	ec_cfg.data_block_count = 2; /* data block count */
	ec_cfg.rdnc_block_count = 2; /* redundancy block count */
	ec_cfg.delete_block_indices[0] = 0;
	ec_cfg.n_delete_block = 1;
	ec_cfg.num_dst_buf = 1;
	ec_cfg.num_src_buf = 1;

	result = doca_argp_init(NULL, &ec_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init ARGP resources: %s", doca_error_get_descr(result));
		goto sample_exit;
	}

	result = register_ec_params();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register ARGP params: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = doca_argp_start(argc, argv);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to parse sample input: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	result = ec_recover(ec_cfg.pci_address,
			    ec_cfg.input_path,
			    ec_cfg.output_path,
			    ec_cfg.do_both,
			    ec_cfg.matrix,
			    ec_cfg.data_block_count,
			    ec_cfg.rdnc_block_count,
			    ec_cfg.delete_block_indices,
			    ec_cfg.n_delete_block,
			    ec_cfg.num_src_buf,
			    ec_cfg.num_dst_buf);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("ec_recover() encountered an error: %s", doca_error_get_descr(result));
		goto argp_cleanup;
	}

	exit_status = EXIT_SUCCESS;

argp_cleanup:
	doca_argp_destroy();
sample_exit:
	if (exit_status == EXIT_SUCCESS)
		DOCA_LOG_INFO("Sample finished successfully");
	else
		DOCA_LOG_INFO("Sample finished with errors");
	return exit_status;
}
