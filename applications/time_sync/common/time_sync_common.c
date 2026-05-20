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

#include "time_sync_common.h"

#include <stdint.h>
#include <stdlib.h>

#include <doca_argp.h>
#include <doca_log.h>

DOCA_LOG_REGISTER(TIME_SYNC::TIME_SYNC_COMMON);

doca_error_t time_sync_common_open_dev_with_caps(struct time_sync_cfg *ts_cfg, caps_check cap_func)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0, i;
	uint8_t is_equal = 0;
	doca_error_t result, ret;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return result;
	}

	ret = DOCA_ERROR_NOT_FOUND;

	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_is_equal_pci_addr(dev_list[i], ts_cfg->pci_addr, &is_equal);
		if (result != DOCA_SUCCESS)
			continue;

		if (is_equal == 0)
			continue;

		/* Found the matching address - verify caps */
		if (cap_func != NULL) {
			result = cap_func(dev_list[i]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Cap check on device failed: %s", doca_error_get_descr(result));
				ret = result;
				break;
			}
		}

		result = doca_dev_open(dev_list[i], &ts_cfg->doca_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open doca_device: %s", doca_error_get_descr(result));
			ret = result;
			break;
		}

		/* Device is found and open so stop checking */
		ret = DOCA_SUCCESS;
		break;
	}

	doca_devinfo_destroy_list(dev_list);

	return ret;
}

doca_error_t time_sync_common_open_repr(struct time_sync_cfg *ts_cfg)
{
	struct doca_devinfo_rep **repr_list;
	uint32_t nb_devs = 0, i;
	uint8_t is_equal = 0;
	doca_error_t result, ret;

	result = doca_devinfo_rep_create_list(ts_cfg->doca_dev, DOCA_DEVINFO_REP_FILTER_NET, &repr_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load repr DOCA devices list: %s", doca_error_get_descr(result));
		return result;
	}

	ret = DOCA_ERROR_NOT_FOUND;

	if (nb_devs == 0) {
		DOCA_LOG_ERR("No representor devices detected");
		return ret;
	}

	for (i = 0; i < nb_devs; i++) {
		/* If no repr_addr has been input, try to open the first detected */
		if (ts_cfg->repr_addr[0] != 0) {
			result = doca_devinfo_rep_is_equal_pci_addr(repr_list[i], ts_cfg->repr_addr, &is_equal);
			if (result != DOCA_SUCCESS)
				continue;

			if (is_equal == 0)
				continue;
		}

		result = doca_dev_rep_open(repr_list[i], &ts_cfg->repr_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to open repr device: %s", doca_error_get_descr(result));
			ret = result;
			break;
		}

		/* Device is found and open so stop checking */
		ret = DOCA_SUCCESS;
		break;
	}

	doca_devinfo_rep_destroy_list(repr_list);

	return ret;
}

doca_error_t time_sync_common_close_devs(struct time_sync_cfg *ts_cfg)
{
	doca_error_t result;

	if (ts_cfg->doca_dev != NULL) {
		result = doca_dev_close(ts_cfg->doca_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close doca device: %s", doca_error_get_descr(result));
			return result;
		}
		ts_cfg->doca_dev = NULL;
	}

	if (ts_cfg->repr_dev != NULL) {
		result = doca_dev_rep_close(ts_cfg->repr_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to close repr device: %s", doca_error_get_descr(result));
			return result;
		}
		ts_cfg->repr_dev = NULL;
	}

	return DOCA_SUCCESS;
}

#ifdef DOCA_ARCH_DPU
/*
 * ARGP Callback - Handle Comch DOCA device repr PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dev_repr_addr_callback(void *param, void *config)
{
	struct time_sync_cfg *cfg = (struct time_sync_cfg *)config;
	const char *dev_repr_addr = (char *)param;
	size_t len;

	len = strnlen(dev_repr_addr, DOCA_DEVINFO_REP_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_REP_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device repr PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_REP_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	memcpy(cfg->repr_addr, dev_repr_addr, len);
	cfg->repr_addr[len] = '\0';

	return DOCA_SUCCESS;
}
#else
/*
 * ARGP Callback - Handle host side delay parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t delay_addr_callback(void *param, void *config)
{
	struct time_sync_cfg *cfg = (struct time_sync_cfg *)config;
	/* Define a maximum delay value of 10 seconds (10000 msecs) */
	const uint32_t max_delay_msec = 10000;
	uint32_t delay = *(uint32_t *)param;

	if (delay > max_delay_msec) {
		DOCA_LOG_ERR("Entered delay (%u) should not exceed %u msecs", delay, max_delay_msec);
		return DOCA_ERROR_INVALID_VALUE;
	}

	cfg->delay = delay;

	return DOCA_SUCCESS;
}
#endif

/*
 * ARGP Callback - Handle DOCA device PCI address parameter
 *
 * @param [in]: Input parameter
 * @config [in/out]: Program configuration context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t dev_pci_addr_callback(void *param, void *config)
{
	struct time_sync_cfg *cfg = (struct time_sync_cfg *)config;
	const char *dev_pci_addr = (char *)param;
	size_t len;

	len = strnlen(dev_pci_addr, DOCA_DEVINFO_PCI_ADDR_SIZE);
	if (len == DOCA_DEVINFO_PCI_ADDR_SIZE) {
		DOCA_LOG_ERR("Entered device PCI address exceeding the maximum size of %d",
			     DOCA_DEVINFO_PCI_ADDR_SIZE - 1);
		return DOCA_ERROR_INVALID_VALUE;
	}

	memcpy(cfg->pci_addr, dev_pci_addr, len);
	cfg->pci_addr[len] = '\0';

	return DOCA_SUCCESS;
}

doca_error_t time_sync_common_reg_params(void)
{
	struct doca_argp_param *pci_param;
#ifdef DOCA_ARCH_DPU
	/* Only applicable to DPU side */
	struct doca_argp_param *repr_param;
#else
	/* Only applicable to Host side */
	struct doca_argp_param *delay_param;
#endif
	doca_error_t result;

	/* Create and register DOCA device PCI address param */
	result = doca_argp_param_create(&pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(pci_param, "p");
	doca_argp_param_set_long_name(pci_param, "pci-addr");
	doca_argp_param_set_description(pci_param, "DOCA device PCI address");
	doca_argp_param_set_callback(pci_param, dev_pci_addr_callback);
	doca_argp_param_set_type(pci_param, DOCA_ARGP_TYPE_STRING);
	doca_argp_param_set_mandatory(pci_param);
	result = doca_argp_register_param(pci_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}

#ifdef DOCA_ARCH_DPU
	/* Create and register DOCA device representor PCI address param */
	result = doca_argp_param_create(&repr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(repr_param, "r");
	doca_argp_param_set_long_name(repr_param, "repr-addr");
	doca_argp_param_set_description(repr_param, "DOCA device representor PCI address (optional)");
	doca_argp_param_set_callback(repr_param, dev_repr_addr_callback);
	doca_argp_param_set_type(repr_param, DOCA_ARGP_TYPE_STRING);
	result = doca_argp_register_param(repr_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}
#else
	/* Create and register delay param */
	result = doca_argp_param_create(&delay_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create ARGP param: %s", doca_error_get_descr(result));
		return result;
	}
	doca_argp_param_set_short_name(delay_param, "d");
	doca_argp_param_set_long_name(delay_param, "delay");
	doca_argp_param_set_description(delay_param, "Delay (msecs) to insert between event triggers (optional)");
	doca_argp_param_set_callback(delay_param, delay_addr_callback);
	doca_argp_param_set_type(delay_param, DOCA_ARGP_TYPE_INT);
	result = doca_argp_register_param(delay_param);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register program param: %s", doca_error_get_descr(result));
		return result;
	}
#endif

	return DOCA_SUCCESS;
}

doca_error_t time_sync_common_create_clock(struct time_sync_cfg *ts_cfg)
{
	doca_error_t result;

	result = doca_clock_create(ts_cfg->doca_dev, &ts_cfg->clock);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to create DOCA clock: %s", doca_error_get_descr(result));

	return result;
}

doca_error_t time_sync_common_destroy_clock(struct time_sync_cfg *ts_cfg)
{
	doca_error_t result;

	result = doca_clock_destroy(ts_cfg->clock);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy DOCA clock: %s", doca_error_get_descr(result));

	ts_cfg->clock = NULL;

	return result;
}
