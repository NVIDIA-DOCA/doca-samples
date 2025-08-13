/*
 * Copyright (c) 2022-2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "common.h"

DOCA_LOG_REGISTER(COMMON);

doca_error_t open_doca_device_with_pci_and_callback(const char *pci_addr,
						    tasks_check func,
						    open_dev_cb open_dev_cb,
						    void *usr_ctx,
						    struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	uint8_t is_addr_equal = 0;
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_is_equal_pci_addr(dev_list[i], pci_addr, &is_addr_equal);
		if (res == DOCA_SUCCESS && is_addr_equal) {
			/* If any special capabilities are needed */
			if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
				continue;

			/* if device can be opened */
			if (open_dev_cb != NULL) {
				res = open_dev_cb(dev_list[i], usr_ctx, retval);
				if (res == DOCA_SUCCESS) {
					doca_devinfo_destroy_list(dev_list);
					return res;
				}
			}
			res = doca_dev_open(dev_list[i], retval);
			if (res == DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				return res;
			}
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_pci(const char *pci_addr, tasks_check func, struct doca_dev **retval)
{
	return open_doca_device_with_pci_and_callback(pci_addr, func, NULL, NULL, retval);
}

doca_error_t open_doca_device_with_ibdev_name(const uint8_t *value,
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	char buf[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {};
	char val_copy[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {};
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	/* Setup */
	if (val_size > DOCA_DEVINFO_IBDEV_NAME_SIZE) {
		DOCA_LOG_ERR("Value size too large. Failed to locate device");
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(val_copy, value, val_size);

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_ibdev_name(dev_list[i], buf, DOCA_DEVINFO_IBDEV_NAME_SIZE);
		if (res == DOCA_SUCCESS && strncmp(buf, val_copy, val_size) == 0) {
			/* If any special capabilities are needed */
			if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
				continue;

			/* if device can be opened */
			res = doca_dev_open(dev_list[i], retval);
			if (res == DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				return res;
			}
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_iface_name(const uint8_t *value,
					      size_t val_size,
					      tasks_check func,
					      struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	char buf[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
	char val_copy[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	/* Setup */
	if (val_size > DOCA_DEVINFO_IFACE_NAME_SIZE) {
		DOCA_LOG_ERR("Value size too large. Failed to locate device");
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(val_copy, value, val_size);

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_iface_name(dev_list[i], buf, DOCA_DEVINFO_IFACE_NAME_SIZE);
		if (res == DOCA_SUCCESS && strncmp(buf, val_copy, val_size) == 0) {
			/* If any special capabilities are needed */
			if (func != NULL && func(dev_list[i]) != DOCA_SUCCESS)
				continue;

			/* if device can be opened */
			res = doca_dev_open(dev_list[i], retval);
			if (res == DOCA_SUCCESS) {
				doca_devinfo_destroy_list(dev_list);
				return res;
			}
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_sf_index(uint32_t sf_index, tasks_check func, struct doca_dev **retval)
{
	enum doca_pci_func_type pci_func_type;
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	uint32_t sf_idx;
	doca_error_t res;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	res = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(res));
		return res;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		res = doca_devinfo_get_pci_func_type(dev_list[i], &pci_func_type);
		if (res == DOCA_SUCCESS && pci_func_type != DOCA_PCI_FUNC_TYPE_SF)
			continue;

		res = doca_devinfo_get_sf_index(dev_list[i], &sf_idx);
		if (res == DOCA_SUCCESS && sf_idx == sf_index) {
			/* If any special capabilities are needed */
			if (func != NULL) {
				res = func(dev_list[i]);
				if (res != DOCA_SUCCESS)
					goto end;
			}

			/* if device can be opened */
			res = doca_dev_open(dev_list[i], retval);
			if (res != DOCA_SUCCESS)
				DOCA_LOG_WARN("Failed open device with SF index %u: %s",
					      sf_index,
					      doca_error_get_descr(res));
			goto end;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	res = DOCA_ERROR_NOT_FOUND;

end:
	doca_devinfo_destroy_list(dev_list);
	return res;
}

doca_error_t open_doca_device_with_capabilities(tasks_check func, struct doca_dev **retval)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t result;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load doca devices list: %s", doca_error_get_descr(result));
		return result;
	}

	/* Search */
	for (i = 0; i < nb_devs; i++) {
		/* If any special capabilities are needed */
		if (func(dev_list[i]) != DOCA_SUCCESS)
			continue;

		/* If device can be opened */
		if (doca_dev_open(dev_list[i], retval) == DOCA_SUCCESS) {
			doca_devinfo_destroy_list(dev_list);
			return DOCA_SUCCESS;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	doca_devinfo_destroy_list(dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t open_doca_device_rep_with_vuid(struct doca_dev *local,
					    enum doca_devinfo_rep_filter filter,
					    const uint8_t *value,
					    size_t val_size,
					    struct doca_dev_rep **retval)
{
	uint32_t nb_rdevs = 0;
	struct doca_devinfo_rep **rep_dev_list = NULL;
	char val_copy[DOCA_DEVINFO_REP_VUID_SIZE] = {};
	char buf[DOCA_DEVINFO_REP_VUID_SIZE] = {};
	doca_error_t result;
	size_t i;

	/* Set default return value */
	*retval = NULL;

	/* Setup */
	if (val_size > DOCA_DEVINFO_REP_VUID_SIZE) {
		DOCA_LOG_ERR("Value size too large. Ignored");
		return DOCA_ERROR_INVALID_VALUE;
	}
	memcpy(val_copy, value, val_size);

	/* Search */
	result = doca_devinfo_rep_create_list(local, filter, &rep_dev_list, &nb_rdevs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create devinfo representor list. Representor devices are available only on DPU, do not run on Host");
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (i = 0; i < nb_rdevs; i++) {
		result = doca_devinfo_rep_get_vuid(rep_dev_list[i], buf, DOCA_DEVINFO_REP_VUID_SIZE);
		if (result == DOCA_SUCCESS && strncmp(buf, val_copy, DOCA_DEVINFO_REP_VUID_SIZE) == 0 &&
		    doca_dev_rep_open(rep_dev_list[i], retval) == DOCA_SUCCESS) {
			doca_devinfo_rep_destroy_list(rep_dev_list);
			return DOCA_SUCCESS;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	doca_devinfo_rep_destroy_list(rep_dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t open_doca_device_rep_with_pci(struct doca_dev *local,
					   enum doca_devinfo_rep_filter filter,
					   const char *pci_addr,
					   struct doca_dev_rep **retval)
{
	uint32_t nb_rdevs = 0;
	struct doca_devinfo_rep **rep_dev_list = NULL;
	uint8_t is_addr_equal = 0;
	doca_error_t result;
	size_t i;

	*retval = NULL;

	/* Search */
	result = doca_devinfo_rep_create_list(local, filter, &rep_dev_list, &nb_rdevs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create devinfo representors list. Representor devices are available only on DPU, do not run on Host");
		return DOCA_ERROR_INVALID_VALUE;
	}

	for (i = 0; i < nb_rdevs; i++) {
		result = doca_devinfo_rep_is_equal_pci_addr(rep_dev_list[i], pci_addr, &is_addr_equal);
		if (result == DOCA_SUCCESS && is_addr_equal &&
		    doca_dev_rep_open(rep_dev_list[i], retval) == DOCA_SUCCESS) {
			doca_devinfo_rep_destroy_list(rep_dev_list);
			return DOCA_SUCCESS;
		}
	}

	DOCA_LOG_WARN("Matching device not found");
	doca_devinfo_rep_destroy_list(rep_dev_list);
	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t create_core_objects(struct program_core_objects *state, uint32_t max_bufs)
{
	doca_error_t res;

	res = doca_mmap_create(&state->src_mmap);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create source mmap: %s", doca_error_get_descr(res));
		return res;
	}
	res = doca_mmap_add_dev(state->src_mmap, state->dev);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add device to source mmap: %s", doca_error_get_descr(res));
		goto destroy_src_mmap;
	}

	res = doca_mmap_create(&state->dst_mmap);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create destination mmap: %s", doca_error_get_descr(res));
		goto destroy_src_mmap;
	}
	res = doca_mmap_add_dev(state->dst_mmap, state->dev);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to add device to destination mmap: %s", doca_error_get_descr(res));
		goto destroy_dst_mmap;
	}

	if (max_bufs != 0) {
		res = doca_buf_inventory_create(max_bufs, &state->buf_inv);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create buffer inventory: %s", doca_error_get_descr(res));
			goto destroy_dst_mmap;
		}

		res = doca_buf_inventory_start(state->buf_inv);
		if (res != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to start buffer inventory: %s", doca_error_get_descr(res));
			goto destroy_buf_inv;
		}
	}

	res = doca_pe_create(&state->pe);
	if (res != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create progress engine: %s", doca_error_get_descr(res));
		goto destroy_buf_inv;
	}

	return DOCA_SUCCESS;

destroy_buf_inv:
	if (state->buf_inv != NULL) {
		doca_buf_inventory_destroy(state->buf_inv);
		state->buf_inv = NULL;
	}

destroy_dst_mmap:
	doca_mmap_destroy(state->dst_mmap);
	state->dst_mmap = NULL;

destroy_src_mmap:
	doca_mmap_destroy(state->src_mmap);
	state->src_mmap = NULL;

	return res;
}

doca_error_t request_stop_ctx(struct doca_pe *pe, struct doca_ctx *ctx)
{
	doca_error_t tmp_result, result = DOCA_SUCCESS;

	tmp_result = doca_ctx_stop(ctx);
	if (tmp_result == DOCA_ERROR_IN_PROGRESS) {
		enum doca_ctx_states ctx_state;

		do {
			(void)doca_pe_progress(pe);
			tmp_result = doca_ctx_get_state(ctx, &ctx_state);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_ERROR_PROPAGATE(result, tmp_result);
				DOCA_LOG_ERR("Failed to get state from ctx: %s", doca_error_get_descr(tmp_result));
				break;
			}
		} while (ctx_state != DOCA_CTX_STATE_IDLE);
	} else if (tmp_result != DOCA_SUCCESS) {
		DOCA_ERROR_PROPAGATE(result, tmp_result);
		DOCA_LOG_ERR("Failed to stop ctx: %s", doca_error_get_descr(tmp_result));
	}

	return result;
}

doca_error_t destroy_core_objects(struct program_core_objects *state)
{
	doca_error_t tmp_result, result = DOCA_SUCCESS;

	if (state->pe != NULL) {
		tmp_result = doca_pe_destroy(state->pe);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to destroy pe: %s", doca_error_get_descr(tmp_result));
		}
		state->pe = NULL;
	}

	if (state->buf_inv != NULL) {
		tmp_result = doca_buf_inventory_destroy(state->buf_inv);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to destroy buf inventory: %s", doca_error_get_descr(tmp_result));
		}
		state->buf_inv = NULL;
	}

	if (state->dst_mmap != NULL) {
		tmp_result = doca_mmap_destroy(state->dst_mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to destroy destination mmap: %s", doca_error_get_descr(tmp_result));
		}
		state->dst_mmap = NULL;
	}

	if (state->src_mmap != NULL) {
		tmp_result = doca_mmap_destroy(state->src_mmap);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to destroy source mmap: %s", doca_error_get_descr(tmp_result));
		}
		state->src_mmap = NULL;
	}

	if (state->dev != NULL) {
		tmp_result = doca_dev_close(state->dev);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_ERROR_PROPAGATE(result, tmp_result);
			DOCA_LOG_ERR("Failed to close device: %s", doca_error_get_descr(tmp_result));
		}
		state->dev = NULL;
	}

	return result;
}

char *hex_dump(const void *data, size_t size)
{
	/*
	 * <offset>:     <Hex bytes: 1-8>        <Hex bytes: 9-16>         <Ascii>
	 * 00000000: 31 32 33 34 35 36 37 38  39 30 61 62 63 64 65 66  1234567890abcdef
	 *    8     2         8 * 3          1          8 * 3         1       16       1
	 */
	const size_t line_size = 8 + 2 + 8 * 3 + 1 + 8 * 3 + 1 + 16 + 1;
	size_t i, j, r, read_index;
	size_t num_lines, buffer_size;
	char *buffer, *write_head;
	unsigned char cur_char, printable;
	char ascii_line[17];
	const unsigned char *input_buffer;

	/* Allocate a dynamic buffer to hold the full result */
	num_lines = (size + 16 - 1) / 16;
	buffer_size = num_lines * line_size + 1;
	buffer = (char *)malloc(buffer_size);
	if (buffer == NULL)
		return NULL;
	write_head = buffer;
	input_buffer = data;
	read_index = 0;

	for (i = 0; i < num_lines; i++) {
		/* Offset */
		snprintf(write_head, buffer_size, "%08lX: ", i * 16);
		write_head += 8 + 2;
		buffer_size -= 8 + 2;
		/* Hex print - 2 chunks of 8 bytes */
		for (r = 0; r < 2; r++) {
			for (j = 0; j < 8; j++) {
				/* If there is content to print */
				if (read_index < size) {
					cur_char = input_buffer[read_index++];
					snprintf(write_head, buffer_size, "%02X ", cur_char);
					/* Printable chars go "as-is" */
					if (' ' <= cur_char && cur_char <= '~')
						printable = cur_char;
					/* Otherwise, use a '.' */
					else
						printable = '.';
					/* Else, just use spaces */
				} else {
					snprintf(write_head, buffer_size, "   ");
					printable = ' ';
				}
				ascii_line[r * 8 + j] = printable;
				write_head += 3;
				buffer_size -= 3;
			}
			/* Spacer between the 2 hex groups */
			snprintf(write_head, buffer_size, " ");
			write_head += 1;
			buffer_size -= 1;
		}
		/* Ascii print */
		ascii_line[16] = '\0';
		snprintf(write_head, buffer_size, "%s\n", ascii_line);
		write_head += 16 + 1;
		buffer_size -= 16 + 1;
	}
	/* No need for the last '\n' */
	write_head[-1] = '\0';
	return buffer;
}

uint64_t align_up_uint64(uint64_t value, uint64_t alignment)
{
	uint64_t remainder = (value % alignment);

	if (remainder == 0)
		return value;

	return value + (alignment - remainder);
}

uint64_t align_down_uint64(uint64_t value, uint64_t alignment)
{
	return value - (value % alignment);
}

uint32_t align_up_uint32(uint32_t value, uint32_t alignment)
{
	uint64_t remainder = (value % alignment);
	if (remainder == 0)
		return value;
	return (uint32_t)(value + (alignment - remainder));
}

uint64_t next_power_of_two(uint64_t x)
{
	x--;
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x |= x >> 32;
	return x + 1;
}

doca_error_t allocat_doca_buf_list(struct doca_buf_inventory *buf_inv,
				   struct doca_mmap *mmap,
				   void *buf_addr,
				   size_t buf_len,
				   int num_buf,
				   bool set_data_pos,
				   struct doca_buf **dbuf)
{
	int i = 0;
	size_t other_seg_len = buf_len / num_buf;
	size_t first_seg_len = other_seg_len + (buf_len % num_buf);
	doca_error_t result = DOCA_SUCCESS;
	struct doca_buf *tmp_dbuf = NULL;
	size_t seg_len = first_seg_len;
	void *seg_addr = buf_addr;

	if (buf_inv == NULL) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Invalid value found, doca_buf_inventory is NULL: %s", doca_error_get_descr(result));
		return result;
	}
	if (mmap == NULL) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Invalid value found, doca_mmap is NULL: %s", doca_error_get_descr(result));
		return result;
	}
	if (buf_addr == NULL) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Invalid value found, buf_addr is NULL: %s", doca_error_get_descr(result));
		return result;
	}
	if (buf_len == 0) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Invalid value found, buf_len is 0: %s", doca_error_get_descr(result));
		return result;
	}
	if (num_buf <= 0) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Invalid value found, num_buf is <= 0: %s", doca_error_get_descr(result));
		return result;
	}
	if (dbuf == NULL) {
		result = DOCA_ERROR_INVALID_VALUE;
		DOCA_LOG_ERR("Invalid value found, dbuf is NULL: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < num_buf; i++) {
		if (i > 0) {
			seg_addr += seg_len;
			seg_len = other_seg_len;
			if (seg_len == 0) {
				break;
			}
		}
		result = doca_buf_inventory_buf_get_by_addr(buf_inv, mmap, seg_addr, seg_len, &tmp_dbuf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to acquire DOCA buffer: %s", doca_error_get_descr(result));
			return result;
		}
		if (set_data_pos == true) {
			result = doca_buf_set_data(tmp_dbuf, seg_addr, seg_len);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to set data for DOCA buffer: %s", doca_error_get_descr(result));
				return result;
			}
		}
		if (i == 0) {
			*dbuf = tmp_dbuf;
		} else {
			result = doca_buf_chain_list(*dbuf, tmp_dbuf);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to construct doca_buf chain: %s", doca_error_get_descr(result));
				return result;
			}
		}
	}

	return result;
}
