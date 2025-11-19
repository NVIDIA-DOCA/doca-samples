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

#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include <dpa_common.h>
#include <dpa_nvqual_common_defs.h>
#include <doca_dpa.h>
#include <doca_dev.h>

DOCA_LOG_REGISTER(DPA_NVQUAL);

/**
 * kernel/RPC declaration
 */
doca_dpa_func_t dpa_nvqual_memory_stress_kernel;
doca_dpa_func_t dpa_nvqual_arithmetic_stress_kernel;
doca_dpa_func_t dpa_nvqual_entry_point;

static struct doca_log_backend *stdout_logger = NULL;

static bool interrupted = false;

/**
 * @brief Signal interrupt handler
 *
 * This function updates interrupted flag
 *
 * @signum [in]: signal number
 */
static void sigint_handler(int signum)
{
	(void)signum;
	DOCA_LOG_ERR("Got external interrupt, the program will terminate shortly, please wait");
	interrupted = true;
}

/**
 * @brief Get available EUs
 *
 * This function indicates in a boolian array which EU is available
 *
 * @dpa [in]: Doca DPA struct
 * @total_num_eus [in]: Total number of possible EUs
 * @available_eus [out]: Boolian array of EUs filled with 'true' if availble
 * @available_eus_size [out]: Size of available EUs
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t get_available_eus(struct doca_dpa *dpa,
				      unsigned int total_num_eus,
				      bool *available_eus,
				      unsigned int *available_eus_size)
{
	struct doca_dpa_eu_affinity *affinity = NULL;

	doca_error_t doca_err = DOCA_SUCCESS;
	doca_err = doca_dpa_eu_affinity_create(dpa, &affinity);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA affinity: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	for (unsigned int eu_id = 1; eu_id < total_num_eus; ++eu_id) {
		doca_err = doca_dpa_eu_affinity_set(affinity, eu_id);
		if (doca_err != DOCA_SUCCESS) {
			(void)doca_dpa_eu_affinity_destroy(affinity);
			DOCA_LOG_ERR("Failed to set EU affinity: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		struct doca_dpa_thread *thread = NULL;

		doca_err = doca_dpa_thread_create(dpa, &thread);
		if (doca_err != DOCA_SUCCESS) {
			(void)doca_dpa_eu_affinity_destroy(affinity);
			DOCA_LOG_ERR("Failed to create thread: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_err = doca_dpa_thread_set_affinity(thread, affinity);
		if (doca_err != DOCA_SUCCESS) {
			(void)doca_dpa_eu_affinity_destroy(affinity);
			(void)doca_dpa_thread_destroy(thread);
			DOCA_LOG_ERR("Failed to set thread affinity: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		// Set the kernel function based on the EU id (16 EUs per core).
		// On each core, the first DPA_NVQUAL_NUM_MEMORY_EUS_PER_CORE EUs id will perform memory stress, and the
		// rest will perform arithmetic stress
		bool is_memory_kernel = (eu_id % DPA_NVQUAL_NUM_EUS_PER_CORE < DPA_NVQUAL_NUM_MEMORY_EUS_PER_CORE);
		if (is_memory_kernel) {
			doca_err = doca_dpa_thread_set_func_arg(thread, dpa_nvqual_memory_stress_kernel, 0);
		} else {
			doca_err = doca_dpa_thread_set_func_arg(thread, dpa_nvqual_arithmetic_stress_kernel, 0);
		}

		if (doca_err != DOCA_SUCCESS) {
			(void)doca_dpa_eu_affinity_destroy(affinity);
			(void)doca_dpa_thread_destroy(thread);
			DOCA_LOG_ERR("Failed to set thread kernel: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		/* Suppress stderr prints temporarily to avoid FlexIO prints, as we allow thread failures on setup */
		int orig_stderr_fd = dup(STDERR_FILENO);
		int null_fd = open("/dev/null", O_WRONLY);
		if (orig_stderr_fd != -1 && null_fd != -1) {
			fflush(stderr);
			dup2(null_fd, STDERR_FILENO);
		}
		if (null_fd != -1)
			close(null_fd);

		/* Suppress DOCA SDK error prints temporarily as well */
		(void)doca_log_backend_set_sdk_level(stdout_logger, DOCA_LOG_LEVEL_CRIT);

		doca_err = doca_dpa_thread_start(thread);
		if (doca_err == DOCA_SUCCESS) {
			available_eus[eu_id] = true;
			*available_eus_size = *available_eus_size + 1;
		}

		/* Restore stderr */
		if (orig_stderr_fd != -1) {
			dup2(orig_stderr_fd, STDERR_FILENO);
			close(orig_stderr_fd);
		}

		/* Restore DOCA SDK error prints */
		(void)doca_log_backend_set_sdk_level(stdout_logger, DOCA_LOG_LEVEL_WARNING);

		(void)doca_dpa_thread_stop(thread);
		(void)doca_dpa_thread_destroy(thread);
	}

	(void)doca_dpa_eu_affinity_destroy(affinity);

	return DOCA_SUCCESS;
}

/**
 * @brief Get number of used EUs
 *
 * This function returns the number of used EUs
 *
 * @available_eus [in]: Boolian array of availble EUs
 * @excluded_eus [in]: Boolian array of excluded EUs
 * @total_num_eus [in]: Total number of possible EUs
 * @return: Number of used EUs
 */
static size_t get_num_used_eus(bool *available_eus, bool *excluded_eus, unsigned int total_num_eus)
{
	size_t ret = 0;
	for (unsigned int eu_id = 1; eu_id < total_num_eus; eu_id++) {
		if ((available_eus[eu_id] == true) && (excluded_eus[eu_id] == false)) {
			++ret;
		}
	}
	return ret;
}

/**
 * @brief Iteration complete updates
 *
 * This function updates the DPA nvqual's average latency of single operation when an iteration is completed
 *
 * @nvq [in]: DPA nvqual struct
 * @ret_val [in]: return value
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t iteration_complete_cb(struct dpa_nvqual *nvq, uint64_t ret_val)
{
	// Calculate average based on actual bytes processed per operation (256 bytes for memory kernel)
	float avg = ret_val / (float)(DPA_NVQUAL_MEMORY_BYTES_PER_OP * nvq->num_ops * DPA_NVQUAL_DPA_FREQ);
	nvq->avg_latency_single_op = (nvq->data_size * (float)nvq->avg_latency_single_op + avg) / (nvq->data_size + 1);
	nvq->data_size++;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP Callback - Handle device name parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t dev_name_param_callback(void *param, void *config)
{
	struct dpa_nvqual_config *dpa_cfg = (struct dpa_nvqual_config *)config;
	char *device_name = (char *)param;
	int len;

	len = strnlen(device_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
	if (len == DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Entered device name exceeding the maximum size of %d", DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}
	strncpy(dpa_cfg->dev_name, device_name, len + 1);

	return DOCA_SUCCESS;
}

/**
 * @brief ARGP Callback - Handle duration seconds parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t test_duration_sec_param_callback(void *param, void *config)
{
	struct dpa_nvqual_config *dpa_cfg = (struct dpa_nvqual_config *)config;
	unsigned long err = strtoul((char *)param, NULL, 10);
	if (err == 0) {
		DOCA_LOG_ERR("Failed to convert ARGP param duration seconds to unsigned long");
		return DOCA_ERROR_INVALID_VALUE;
	}
	dpa_cfg->test_duration_sec = err;
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP Callback - Handle user factor parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t user_factor_param_callback(void *param, void *config)
{
	struct dpa_nvqual_config *dpa_cfg = (struct dpa_nvqual_config *)config;
	float user_factor = strtof((char *)param, NULL);
	if (user_factor <= (float)0 || user_factor > (float)1) {
		DOCA_LOG_ERR("User factor parameter is out of range. Expected range: (0, 1]");
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(&dpa_cfg->user_factor, &user_factor, sizeof(dpa_cfg->user_factor));
	return DOCA_SUCCESS;
}

/**
 * @brief ARGP Callback - Handle excluded eus parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_INVALID_VALUE otherwise
 */
static doca_error_t excluded_eus_param_callback(void *param, void *config)
{
	struct dpa_nvqual_config *dpa_cfg = (struct dpa_nvqual_config *)config;
	char *input = (char *)param;
	int len;

	len = strnlen(input, DPA_NVQUAL_MAX_INPUT_EXCLUDED_EUS_SIZE);
	if (len == DPA_NVQUAL_MAX_INPUT_EXCLUDED_EUS_SIZE) {
		DOCA_LOG_ERR("Entered excluded EUs exceeding the maximum size of %d",
			     DPA_NVQUAL_MAX_INPUT_EXCLUDED_EUS_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	char *token = strtok(input, ",");
	while (token != NULL) {
		unsigned int eu_id = (unsigned int)strtoul(token, NULL, 0);
		if (eu_id < DPA_NVQUAL_MAX_EUS) {
			dpa_cfg->excluded_eus[eu_id] = true;
			dpa_cfg->excluded_eus_size = dpa_cfg->excluded_eus_size + 1;
		}
		token = strtok(NULL, ",");
	}

	return DOCA_SUCCESS;
}

/**
 * @brief Register DPA nvqual parameters
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpa_nvqual_register_params(void)
{
	doca_error_t err;
	struct doca_argp_param *dev_name, *test_duration_sec, *user_factor, *excluded_eus;

	err = doca_argp_param_create(&dev_name);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(err));
		return err;
	}
	doca_argp_param_set_short_name(dev_name, "pf_dev");
	doca_argp_param_set_long_name(dev_name, "pf-device");
	doca_argp_param_set_arguments(dev_name, "<PF DOCA device name>");
	doca_argp_param_set_description(dev_name, "PF device name that supports DPA (mandatory).");
	doca_argp_param_set_callback(dev_name, dev_name_param_callback);
	doca_argp_param_set_type(dev_name, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(dev_name);
	err = doca_argp_register_param(dev_name);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(err));
		return err;
	}

	err = doca_argp_param_create(&test_duration_sec);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(err));
		return err;
	}
	doca_argp_param_set_short_name(test_duration_sec, "test_dur");
	doca_argp_param_set_long_name(test_duration_sec, "test-duration-sec");
	doca_argp_param_set_arguments(test_duration_sec, "<test duration sec>");
	doca_argp_param_set_description(test_duration_sec, "test duration seconds (mandatory).");
	doca_argp_param_set_callback(test_duration_sec, test_duration_sec_param_callback);
	doca_argp_param_set_type(test_duration_sec, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(test_duration_sec);
	err = doca_argp_register_param(test_duration_sec);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(err));
		return err;
	}

	err = doca_argp_param_create(&user_factor);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(err));
		return err;
	}
	doca_argp_param_set_short_name(user_factor, "user_fac");
	doca_argp_param_set_long_name(user_factor, "user-factor");
	doca_argp_param_set_arguments(user_factor, "[user factor]");
	doca_argp_param_set_description(
		user_factor,
		"user factor from type float in the range of (0, 1] to determine the duration of each iteration (optional). If not provided then set to 0.75f.");
	doca_argp_param_set_callback(user_factor, user_factor_param_callback);
	doca_argp_param_set_type(user_factor, DOCA_ARGP_TYPE_STRING);
	err = doca_argp_register_param(user_factor);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(err));
		return err;
	}

	err = doca_argp_param_create(&excluded_eus);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(err));
		return err;
	}
	doca_argp_param_set_short_name(excluded_eus, "ex_eu");
	doca_argp_param_set_long_name(excluded_eus, "excluded-eus");
	doca_argp_param_set_arguments(excluded_eus, "[excluded eus]");
	doca_argp_param_set_description(
		excluded_eus,
		"excluded eus list, devided by ',' with no spaced (optional). If not provided then no eu will be excluded.");
	doca_argp_param_set_callback(excluded_eus, excluded_eus_param_callback);
	doca_argp_param_set_type(excluded_eus, DOCA_ARGP_TYPE_STRING);
	err = doca_argp_register_param(excluded_eus);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(err));
		return err;
	}

	return DOCA_SUCCESS;
}

/**
 * @brief Print sample's data
 *
 * This function prints sample's data
 *
 * @output [in]: DPA nvqual output struct
 * @flow_cfg [in]: DPA flow configuration struct
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t print_sample_data(struct dpa_nvqual_run_output *output, struct dpa_nvqual_flow_config *flow_cfg)
{
	char stream[DPA_NVQUAL_PRINT_BUFFER_SIZE];
	stream[0] = '\0';
	bool used_eus[DPA_NVQUAL_MAX_EUS] = {false};
	unsigned int size_used_eus = 0;
	bool unused_eus[DPA_NVQUAL_MAX_EUS] = {false};
	unsigned int size_unused_eus = 0;
	bool unreachable_eus[DPA_NVQUAL_MAX_EUS] = {false};

	unsigned int size_unreachable_eus = output->total_num_eus - 1;
	for (unsigned int eu_id = 1; eu_id < output->total_num_eus; ++eu_id)
		unreachable_eus[eu_id] = true;

	strcat(stream, "{\"data\": {\n");
	snprintf(stream + strlen(stream),
		 DPA_NVQUAL_PRINT_BUFFER_SIZE - strlen(stream),
		 "\t\"device\": \"%s\",\n",
		 flow_cfg->dev_name);
	snprintf(stream + strlen(stream),
		 DPA_NVQUAL_PRINT_BUFFER_SIZE - strlen(stream),
		 "\t\"total_harts\": %d,\n",
		 output->total_num_eus);

	strcat(stream, "\t\"available_harts\": [");
	for (unsigned int eu_id = 1; eu_id < output->total_num_eus; ++eu_id) {
		if (output->available_eus[eu_id] == true) {
			output->available_eus_size--;
			unreachable_eus[eu_id] = false;
			size_unreachable_eus--;
			snprintf(stream + strlen(stream), DPA_NVQUAL_PRINT_BUFFER_SIZE - strlen(stream), "%d", eu_id);
			if (output->available_eus_size > 0) {
				strcat(stream, ",");
			}
			if (flow_cfg->excluded_eus[eu_id] == false) {
				used_eus[eu_id] = true;
				size_used_eus++;
			} else {
				unused_eus[eu_id] = true;
				size_unused_eus++;
			}
		}
	}
	strcat(stream, "],\n");

	strcat(stream, "\t\"unreachable_harts\": [");
	for (unsigned int eu_id = 1; eu_id < output->total_num_eus; ++eu_id) {
		if (unreachable_eus[eu_id] == true) {
			size_unreachable_eus--;
			snprintf(stream + strlen(stream), DPA_NVQUAL_PRINT_BUFFER_SIZE - strlen(stream), "%d", eu_id);
			if (size_unreachable_eus > 0) {
				strcat(stream, ",");
			}
		}
	}
	strcat(stream, "],\n");

	strcat(stream, "\t\"used_harts\": [");
	for (unsigned int eu_id = 1; eu_id < output->total_num_eus; ++eu_id) {
		if (used_eus[eu_id] == true) {
			size_used_eus--;
			snprintf(stream + strlen(stream), DPA_NVQUAL_PRINT_BUFFER_SIZE - strlen(stream), "%d", eu_id);
			if (size_used_eus > 0) {
				strcat(stream, ",");
			}
		}
	}
	strcat(stream, "],\n");

	strcat(stream, "\t\"unused_harts\": [");
	for (unsigned int eu_id = 1; eu_id < output->total_num_eus; ++eu_id) {
		if (unused_eus[eu_id] == true) {
			size_unused_eus--;
			snprintf(stream + strlen(stream), DPA_NVQUAL_PRINT_BUFFER_SIZE - strlen(stream), "%d", eu_id);
			if (size_unused_eus > 0) {
				strcat(stream, ",");
			}
		}
	}
	strcat(stream, "],\n");

	snprintf(stream + strlen(stream),
		 DPA_NVQUAL_PRINT_BUFFER_SIZE - strlen(stream),
		 "\t\"total_dpa_run_time\": \"%ld [sec]\"\n}}\n",
		 output->total_dpa_run_time_sec);
	DOCA_LOG_INFO("\n\n%s\n", stream);

	return DOCA_SUCCESS;
}

/**
 * @brief Setup sample's configuration
 *
 * This function sets up DPA nvqual configuration
 *
 * @nvqual_argp_cfg [in]: DPA nvqual argp configuration
 * @nvq [in]: DPA nvqual struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t setup(struct dpa_nvqual_config *nvqual_argp_cfg, struct dpa_nvqual *nvq)
{
	float float_user_factor;
	memcpy(&float_user_factor, &nvqual_argp_cfg->user_factor, sizeof(float_user_factor));

	struct dpa_nvqual_flow_config flow_cfg;
	uint64_t iteration_duration_sec_var = DPA_NVQUAL_ITERATION_DURATION_SEC;
	if (float_user_factor != (float)0) {
		iteration_duration_sec_var = DPA_NVQUAL_WATCHDOG_TIME_SEC * float_user_factor;
	}
	for (int i = 0; i < DPA_NVQUAL_MAX_EUS; i++) {
		flow_cfg.excluded_eus[i] = nvqual_argp_cfg->excluded_eus[i];
	}
	flow_cfg.excluded_eus_size = nvqual_argp_cfg->excluded_eus_size;
	flow_cfg.dev_name = nvqual_argp_cfg->dev_name;
	flow_cfg.test_duration_sec = nvqual_argp_cfg->test_duration_sec;
	flow_cfg.iteration_duration_sec = iteration_duration_sec_var;
	flow_cfg.allocated_dpa_heap_size = DPA_NVQUAL_ALLOCATED_DPA_HEAP_SIZE;

	nvq->flow_cfg = flow_cfg;
	nvq->dev_list = NULL;
	nvq->dev = NULL;
	nvq->dpa = NULL;
	nvq->affinity = NULL;
	nvq->se = NULL;
	nvq->num_threads = 0;
	nvq->available_eus_size = 0;
	nvq->total_num_eus = 0;
	nvq->buffers_size = 0;
	nvq->num_ops = 0;
	nvq->total_dpa_run_time_usec = 0;
	nvq->avg_latency_single_op = 0;
	nvq->data_size = 0;
	for (unsigned int eu_id = 0; eu_id < DPA_NVQUAL_MAX_EUS; eu_id++) {
		memset(&nvq->tlss[eu_id], 0, sizeof(struct dpa_nvqual_tls));
		nvq->dev_tlss[eu_id] = 0;
		nvq->threads[eu_id] = NULL;
		nvq->notification_completions[eu_id] = NULL;
		nvq->available_eus[eu_id] = false;
	}

	struct sigaction sa;
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) == -1) {
		DOCA_LOG_ERR("Failed to set SIGINT handler: %s", (doca_error_get_name(DOCA_ERROR_OPERATING_SYSTEM)));
		return DOCA_ERROR_OPERATING_SYSTEM;
	}

	uint32_t num_devs = 0;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};

	doca_error_t doca_err = DOCA_SUCCESS;
	doca_err = doca_devinfo_create_list(&nvq->dev_list, &num_devs);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA device list: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	/* Search device with same dev name*/
	for (uint32_t i = 0; i < num_devs; i++) {
		doca_err = doca_devinfo_get_ibdev_name(nvq->dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get ibdev name: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		if (strlen(nvq->flow_cfg.dev_name) && strcmp(ibdev_name, nvq->flow_cfg.dev_name) != 0)
			continue;

		if (doca_dpa_cap_is_supported(nvq->dev_list[i]) != DOCA_SUCCESS)
			continue;

		doca_err = doca_dev_open(nvq->dev_list[i], &nvq->dev);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open doca device: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		DOCA_LOG_INFO("Using device %s", ibdev_name);
		break;
	}

	if (nvq->dev == NULL) {
		DOCA_LOG_ERR("No device with DPA capabilities: %s", (doca_error_get_name(DOCA_ERROR_NOT_FOUND)));
		return DOCA_ERROR_NOT_FOUND;
	}

	doca_err = doca_dpa_create(nvq->dev, &nvq->dpa);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA DPA context: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_dpa_set_app(nvq->dpa, dpa_sample_app);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA application: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_dpa_start(nvq->dpa);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DPA context: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_sync_event_create(&nvq->se);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create Sync Event: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_sync_event_add_publisher_location_cpu(nvq->se, nvq->dev);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to config Sync Event publisher: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_sync_event_add_subscriber_location_dpa(nvq->se, nvq->dpa);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to config Sync Event subscriber: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_sync_event_start(nvq->se);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start Sync Event: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_sync_event_get_dpa_handle(nvq->se, nvq->dpa, &nvq->dev_se);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get Sync Event device handle: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_dpa_get_total_num_eus_available(nvq->dpa, &nvq->total_num_eus);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get total number of DPA EUs: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = get_available_eus(nvq->dpa, nvq->total_num_eus, nvq->available_eus, &nvq->available_eus_size);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get total number of available DPA EUs: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	size_t num_used_eus = get_num_used_eus(nvq->available_eus, nvq->flow_cfg.excluded_eus, nvq->total_num_eus);
	if (num_used_eus == 0) {
		DOCA_LOG_ERR("Failed to find any available DPA EUs with the current excluded EUs configuration");
		return DOCA_ERROR_BAD_CONFIG;
	}

	nvq->buffers_size = (size_t)((nvq->flow_cfg.allocated_dpa_heap_size / num_used_eus) / 2);

	doca_err = doca_dpa_mem_alloc(nvq->dpa,
				      sizeof(doca_dpa_dev_notification_completion_t) *
					      (nvq->available_eus_size - nvq->flow_cfg.excluded_eus_size),
				      &nvq->dev_notification_completions);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to alloc notification completions array: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_dpa_mem_alloc(nvq->dpa,
				      sizeof(struct dpa_nvqual_thread_ret) *
					      (nvq->available_eus_size - nvq->flow_cfg.excluded_eus_size),
				      &nvq->thread_rets);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to alloc notification completions array: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_dpa_eu_affinity_create(nvq->dpa, &nvq->affinity);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA affinity: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	for (unsigned int eu_id = 1; eu_id < nvq->total_num_eus; eu_id++) {
		/* Check if current EU ID is unavailable */
		if (nvq->available_eus[eu_id] == false) {
			DOCA_LOG_WARN("Skipping unavailable HART #%d, you may use dpaeumgmt tool to enable it", eu_id);
			continue;
		}

		/* Check if current EU ID is excluded by the user */
		if (nvq->flow_cfg.excluded_eus[eu_id] == true) {
			DOCA_LOG_INFO("Skipping excluded HART #%d", eu_id);
			continue;
		}

		DOCA_LOG_INFO("Setting HART #%d", eu_id);

		doca_err = doca_dpa_eu_affinity_set(nvq->affinity, eu_id);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set EU affinity: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		struct doca_dpa_thread *thread = NULL;

		doca_err = doca_dpa_thread_create(nvq->dpa, &thread);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create thread: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_err = doca_dpa_thread_set_affinity(thread, nvq->affinity);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set thread affinity: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_dpa_dev_uintptr_t dst_buf;

		doca_err = doca_dpa_mem_alloc(nvq->dpa, nvq->buffers_size, &dst_buf);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to alloc dst buffer of size %lu : %s",
				     nvq->buffers_size,
				     (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_dpa_dev_uintptr_t dtls;

		doca_err = doca_dpa_mem_alloc(nvq->dpa, sizeof(struct dpa_nvqual_tls), &dtls);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to alloc thread TLS: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		// Determine kernel type first, then assign appropriate num_ops
		bool is_memory_kernel = (eu_id % DPA_NVQUAL_NUM_EUS_PER_CORE < DPA_NVQUAL_NUM_MEMORY_EUS_PER_CORE);

		struct dpa_nvqual_tls htls;
		htls.eu = eu_id;
		htls.thread_ret = nvq->thread_rets + (nvq->num_threads * sizeof(struct dpa_nvqual_thread_ret));
		htls.dst_buf = dst_buf;
		htls.buffers_size = nvq->buffers_size;
		// Assign different num_ops based on kernel type

		// Calculate num_ops for memory kernel based on actual bytes written per operation (256 bytes)
		uint64_t num_ops_memory =
			(uint64_t)((nvq->flow_cfg.iteration_duration_sec * DPA_NVQUAL_SEC_TO_USEC) /
				   (DPA_NVQUAL_COPY_BYTE_LATENCY_USEC * (float)DPA_NVQUAL_MEMORY_BYTES_PER_OP));

		// Calculate num_ops for arithmetic kernel - accounting for the 100x multiplier in the kernel
		// Divide by ARITHMETIC_OPS_MULTIPLIER since the kernel does (num_ops * 100) iterations
		uint64_t num_ops_arithmetic =
			(uint64_t)((nvq->flow_cfg.iteration_duration_sec * DPA_NVQUAL_SEC_TO_USEC *
				    DPA_NVQUAL_DPA_FREQ) /
				   (DPA_NVQUAL_ARITHMETIC_CYCLES_PER_OP * DPA_NVQUAL_ARITHMETIC_OPS_MULTIPLIER));

		htls.num_ops = is_memory_kernel ? num_ops_memory : num_ops_arithmetic;
		htls.dev_se = nvq->dev_se;

		doca_err = doca_dpa_h2d_memcpy(nvq->dpa, dtls, (void *)(&htls), sizeof(struct dpa_nvqual_tls));
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize thread TLS: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		nvq->tlss[eu_id] = htls;
		nvq->dev_tlss[eu_id] = dtls;
		nvq->num_ops = htls.num_ops;

		// Set the kernel function based on the EU id (16 EUs per core).
		// On each core, the first DPA_NVQUAL_NUM_MEMORY_EUS_PER_CORE EUs id will perform memory stress, and the
		// rest will perform arithmetic stress
		if (is_memory_kernel) {
			doca_err = doca_dpa_thread_set_func_arg(thread, dpa_nvqual_memory_stress_kernel, 0);
		} else {
			doca_err = doca_dpa_thread_set_func_arg(thread, dpa_nvqual_arithmetic_stress_kernel, 0);
		}

		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set thread kernel: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_err = doca_dpa_thread_set_local_storage(thread, dtls);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set thread TLS: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_err = doca_dpa_thread_start(thread);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start DPA thread: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		struct doca_dpa_notification_completion *notify_comp = NULL;

		doca_err = doca_dpa_notification_completion_create(nvq->dpa, thread, &notify_comp);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create notification completion: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_err = doca_dpa_notification_completion_start(notify_comp);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start notification completion: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_dpa_dev_notification_completion_t dev_notify_comp;

		doca_err = doca_dpa_notification_completion_get_dpa_handle(notify_comp, &dev_notify_comp);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get notification completion device handle: %s",
				     (doca_error_get_name(doca_err)));
			return doca_err;
		}

		doca_err =
			doca_dpa_h2d_memcpy(nvq->dpa,
					    nvq->dev_notification_completions +
						    (nvq->num_threads * sizeof(doca_dpa_dev_notification_completion_t)),
					    (void *)(&dev_notify_comp),
					    sizeof(doca_dpa_dev_notification_completion_t));
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize thread TLS: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		nvq->notification_completions[eu_id] = notify_comp;

		doca_err = doca_dpa_thread_run(thread);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to run thread: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		nvq->threads[eu_id] = thread;
		nvq->num_threads++;
	}
	return DOCA_SUCCESS;
}

/**
 * @brief Tear down sample's configuration
 *
 * This function tears down DPA nvqual configuration
 *
 * @nvq [in]: DPA nvqual struct
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t tear_down(struct dpa_nvqual *nvq)
{
	for (unsigned int i = 1; i < nvq->total_num_eus; i++) {
		if (nvq->notification_completions[i] != NULL) {
			(void)doca_dpa_notification_completion_stop(nvq->notification_completions[i]);
			(void)doca_dpa_notification_completion_destroy(nvq->notification_completions[i]);
		}
	}

	for (unsigned int i = 1; i < nvq->total_num_eus; i++) {
		if (memcmp(&nvq->tlss[i], &(struct dpa_nvqual_tls){0}, sizeof(struct dpa_nvqual_tls)) != 0) {
			(void)doca_dpa_mem_free(nvq->dpa, nvq->tlss[i].dst_buf);
		}
	}

	for (unsigned int i = 1; i < nvq->total_num_eus; i++) {
		if (nvq->dev_tlss[i] != 0) {
			(void)doca_dpa_mem_free(nvq->dpa, nvq->dev_tlss[i]);
		}
	}

	for (unsigned int i = 1; i < nvq->total_num_eus; i++) {
		if (nvq->threads[i] != NULL) {
			(void)doca_dpa_thread_stop(nvq->threads[i]);
			(void)doca_dpa_thread_destroy(nvq->threads[i]);
		}
	}

	if (nvq->thread_rets)
		(void)doca_dpa_mem_free(nvq->dpa, nvq->thread_rets);

	if (nvq->dev_notification_completions)
		(void)doca_dpa_mem_free(nvq->dpa, nvq->dev_notification_completions);

	if (nvq->affinity)
		(void)doca_dpa_eu_affinity_destroy(nvq->affinity);

	if (nvq->se) {
		(void)doca_sync_event_stop(nvq->se);
		(void)doca_sync_event_destroy(nvq->se);
	}

	if (nvq->dpa) {
		(void)doca_dpa_stop(nvq->dpa);
		(void)doca_dpa_destroy(nvq->dpa);
	}

	if (nvq->dev)
		(void)doca_dev_close(nvq->dev);

	if (nvq->dev_list)
		(void)doca_devinfo_destroy_list(nvq->dev_list);

	return DOCA_SUCCESS;
}

/**
 * @brief Run sample's flow
 *
 * This function runs sample's flow
 *
 * @nvq [in]: DPA nvqual struct
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_flow(struct dpa_nvqual *nvq)
{
	uint64_t iter = 1;
	doca_error_t doca_err = DOCA_SUCCESS;

	long long time_left = nvq->flow_cfg.test_duration_sec * DPA_NVQUAL_SEC_TO_USEC;

	while (time_left > 0) {
		if (interrupted) {
			DOCA_LOG_INFO("Terminating...");
			break;
		}

		int h = time_left / ((unsigned int)DPA_NVQUAL_SEC_IN_HOUR * (unsigned int)DPA_NVQUAL_SEC_TO_USEC);
		int m = ((time_left / DPA_NVQUAL_SEC_TO_USEC) % DPA_NVQUAL_SEC_IN_HOUR) / DPA_NVQUAL_SEC_IN_MINUTE;
		int s = ((time_left / DPA_NVQUAL_SEC_TO_USEC) % DPA_NVQUAL_SEC_IN_HOUR) % DPA_NVQUAL_SEC_IN_MINUTE;
		DOCA_LOG_INFO("Estimated time left: %02d:%02d:%02d", h, m, s);

		DOCA_LOG_INFO("Starting iteration #%ld", iter);

		uint64_t ret = 1;

		struct timeval start;
		gettimeofday(&start, NULL);

		DOCA_LOG_INFO("Dispatching kernel threads on all HARTs");
		doca_err = doca_dpa_rpc(nvq->dpa,
					dpa_nvqual_entry_point,
					&ret,
					nvq->dev_notification_completions,
					nvq->num_threads);
		if (doca_err != DOCA_SUCCESS && ret != 0) {
			DOCA_LOG_ERR("Failed to run RPC for waking up all threads: %s",
				     (doca_error_get_name(doca_err)));
			return doca_err;
		}

		DOCA_LOG_INFO("Waiting for all threads...");
		doca_err = doca_sync_event_wait_gt(nvq->se, nvq->num_threads - 1, DPA_NVQUAL_SYNC_EVENT_MASK);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to wait for all threads: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		struct timeval current;
		gettimeofday(&current, NULL);

		long long start_time = start.tv_sec * DPA_NVQUAL_SEC_TO_USEC + start.tv_usec;
		long long current_time = current.tv_sec * DPA_NVQUAL_SEC_TO_USEC + current.tv_usec;
		long long elapsed = current_time - start_time;

		time_left -= elapsed;
		nvq->total_dpa_run_time_usec = nvq->total_dpa_run_time_usec + elapsed;

		DOCA_LOG_INFO("Retrieving kernel returned values");
		struct dpa_nvqual_thread_ret thread_rets_host[nvq->num_threads];
		doca_err = doca_dpa_d2h_memcpy(nvq->dpa,
					       (void *)(thread_rets_host),
					       nvq->thread_rets,
					       sizeof(struct dpa_nvqual_thread_ret) * nvq->num_threads);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to copy thread rets from DPA heap: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}

		DOCA_LOG_INFO("Reporting interation errs");
		for (unsigned int thread_idx = 0; thread_idx < nvq->num_threads; thread_idx++)
			iteration_complete_cb(nvq, thread_rets_host[thread_idx].val);

		DOCA_LOG_INFO("Preparing for next iteration");
		doca_err = doca_sync_event_update_set(nvq->se, 0);
		if (doca_err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to clear se: %s", (doca_error_get_name(doca_err)));
			return doca_err;
		}
		++iter;
	}

	DOCA_LOG_INFO("DPA run has completed.");

	return DOCA_SUCCESS;
}

/**
 * @brief Run DPA nvqual sample
 *
 * This function runs DPA nvqual sample
 *
 * @nvqual_argp_cfg [in]: DPA nvqual argp configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t dpa_nvqual(struct dpa_nvqual_config *nvqual_argp_cfg)
{
	doca_error_t doca_err = DOCA_SUCCESS;

	doca_err = doca_log_backend_create_with_file_sdk(stdout, &stdout_logger);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create logging backend for SDK messages: %s", (doca_error_get_name(doca_err)));
		return doca_err;
	}

	doca_err = doca_log_backend_set_sdk_level(stdout_logger, DOCA_LOG_LEVEL_WARNING);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set logging backend log level for SDK messages: %s",
			     (doca_error_get_name(doca_err)));
		return doca_err;
	}

#ifdef DOCA_DEBUG
	doca_err = doca_log_backend_set_sdk_level(stdout_logger, DOCA_LOG_LEVEL_DEBUG);
	if (doca_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set logging backend log level for SDK messages: %s",
			     (doca_error_get_name(doca_err)));
		return doca_err;
	}
#endif
	struct dpa_nvqual *nvq = (struct dpa_nvqual *)malloc(sizeof(struct dpa_nvqual));
	if (nvq == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for main context");
		return DOCA_ERROR_NO_MEMORY;
	}

	doca_err = setup(nvqual_argp_cfg, nvq);
	if (doca_err != DOCA_SUCCESS) {
		tear_down(nvq);
		free(nvq);
		return doca_err;
	}

	if (!nvq->dpa) {
		DOCA_LOG_ERR(
			"DPA context member hasn't been initialized during the setup phase. Please initialize it in the setup method.");
		return DOCA_ERROR_BAD_STATE;
	}

	doca_err = run_flow(nvq);
	if (doca_err != DOCA_SUCCESS) {
		tear_down(nvq);
		free(nvq);
		return doca_err;
	}

	struct dpa_nvqual_run_output ret;
	memcpy(ret.available_eus, nvq->available_eus, sizeof(nvq->available_eus));
	ret.available_eus_size = nvq->available_eus_size;
	ret.total_dpa_run_time_sec = (uint64_t)(nvq->total_dpa_run_time_usec / DPA_NVQUAL_SEC_TO_USEC);
	ret.total_num_eus = nvq->total_num_eus;

	print_sample_data(&ret, &(nvq->flow_cfg));

	tear_down(nvq);

	free(nvq);

	return DOCA_SUCCESS;
}
