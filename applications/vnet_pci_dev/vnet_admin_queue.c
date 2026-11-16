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

#include <endian.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <doca_buf.h>
#include <doca_devemu_virtio_io.h>
#include <doca_error.h>
#include <doca_log.h>

#include "virtio_admin_spec.h"
#include "vnet_admin_dev_parts.h"
#include "vnet_admin_queue.h"
#include "vnet_admin_vf.h"
#include "vnet_pci_dev_core.h"
#include "vnet_pci_device.h"

DOCA_LOG_REGISTER(VNET_ADMIN_QUEUE);

#define VNET_ADMIN_SUPPORTED_OPCODES \
	((1ULL << VIRTIO_ADMIN_CMD_LIST_QUERY) | (1ULL << VIRTIO_ADMIN_CMD_LIST_USE) | \
	 (1ULL << VIRTIO_ADMIN_CMD_CAP_ID_LIST_QUERY) | (1ULL << VIRTIO_ADMIN_CMD_DEVICE_CAP_GET) | \
	 (1ULL << VIRTIO_ADMIN_CMD_DRIVER_CAP_SET) | (1ULL << VIRTIO_ADMIN_CMD_RESOURCE_OBJ_CREATE) | \
	 (1ULL << VIRTIO_ADMIN_CMD_RESOURCE_OBJ_DESTROY) | (1ULL << VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_GET) | \
	 (1ULL << VIRTIO_ADMIN_CMD_DEV_PARTS_GET) | (1ULL << VIRTIO_ADMIN_CMD_DEV_PARTS_SET) | \
	 (1ULL << VIRTIO_ADMIN_CMD_DEV_MODE_SET))

enum vnet_admin_async_type {
	VNET_ADMIN_ASYNC_DEV_PARTS_SET,
	VNET_ADMIN_ASYNC_DEV_MODE_SET,
};

struct vnet_admin_async_cmd {
	/* Controller, request, resource, and PF are borrowed while retained_count pins the command. */
	struct vnet_pci_dev_controller *ctrl;
	struct doca_devemu_virtio_admin_req *req;
	struct vnet_admin_resource_obj *obj;
	struct vnet_admin_dev_parts_restore_state *restore_state;
	struct pci_device_config *pf;
	vnet_admin_cmd_complete_cb complete;
	void *complete_context;
	uint64_t generation;
	uint16_t slot_idx;
	enum vnet_admin_async_type type;
	bool stopped;
};

static void vnet_admin_result_set(struct vnet_admin_cmd_result *result, uint16_t qualifier)
{
	result->status_qualifier = qualifier;
	result->status = vnet_admin_status_from_qualifier(qualifier);
	result->len = 0;
	result->deferred = false;
}

static bool vnet_admin_list_target_validate(uint16_t opcode,
					    uint64_t group_member_id,
					    struct vnet_admin_cmd_result *result)
{
	if (group_member_id != 0) {
		DOCA_LOG_ERR("Unsupported Admin VQ group member %" PRIu64 " for opcode %u", group_member_id, opcode);
		result->status = VNET_ADMIN_STATUS_ERR;
		result->status_qualifier = VIRTIO_ADMIN_STATUS_Q_INVALID_MEMBER;
		return false;
	}

	return true;
}

/* vnet AQ currently exposes one doca_buf because vnet_ctrl supplies one IOV, while
 * list traversal preserves the generic admin-request buffer-list contract. */
bool vnet_admin_buf_list_copy(struct doca_buf *buf, void *data, uint32_t len, bool to_buf)
{
	struct doca_buf *curr_buf = buf;
	void *curr_data;
	doca_error_t result;
	size_t curr_len;
	uint32_t processed = 0, chunk;

	while (curr_buf != NULL && processed < len) {
		result = doca_buf_get_data_len(curr_buf, &curr_len);
		if (result != DOCA_SUCCESS)
			break;
		result = doca_buf_get_data(curr_buf, &curr_data);
		if (result != DOCA_SUCCESS || curr_data == NULL)
			break;

		chunk = (len - processed < curr_len) ? (len - processed) : (uint32_t)curr_len;
		if (to_buf)
			memcpy(curr_data, (uint8_t *)data + processed, chunk);
		else
			memcpy((uint8_t *)data + processed, curr_data, chunk);
		processed += chunk;

		if (processed >= len)
			break;

		if (doca_buf_get_next_in_list(curr_buf, &curr_buf) != DOCA_SUCCESS)
			break;
	}

	return processed == len;
}

void vnet_admin_cmd_view_init_plain(struct vnet_admin_cmd_view *view,
				    const void *data,
				    uint32_t data_len,
				    void *result,
				    uint32_t result_len,
				    vnet_admin_cmd_complete_cb complete,
				    void *complete_context)
{
	memset(view, 0, sizeof(*view));
	view->data = data;
	view->result = result;
	view->complete = complete;
	view->complete_context = complete_context;
	view->data_len = data_len;
	view->result_len = result_len;
	view->buffer_type = VNET_ADMIN_CMD_BUFFER_PLAIN;
}

bool vnet_admin_cmd_view_read(const struct vnet_admin_cmd_view *view, void *data, uint32_t len)
{
	if (len > view->data_len)
		return false;
	if (view->buffer_type == VNET_ADMIN_CMD_BUFFER_DOCA)
		return vnet_admin_buf_list_copy(view->data_doca_buf, data, len, false);
	if (view->data == NULL)
		return len == 0;
	memcpy(data, view->data, len);
	return true;
}

bool vnet_admin_cmd_view_write(const struct vnet_admin_cmd_view *view, const void *data, uint32_t len)
{
	if (len > view->result_len)
		return false;
	if (view->buffer_type == VNET_ADMIN_CMD_BUFFER_DOCA)
		return vnet_admin_buf_list_copy(view->result_doca_buf, (void *)data, len, true);
	if (view->result == NULL)
		return len == 0;
	memcpy(view->result, data, len);
	return true;
}

bool vnet_admin_cmd_view_has_data(const struct vnet_admin_cmd_view *view)
{
	return view->buffer_type == VNET_ADMIN_CMD_BUFFER_DOCA ? view->data_doca_buf != NULL : view->data != NULL;
}

bool vnet_admin_cmd_view_has_result(const struct vnet_admin_cmd_view *view)
{
	return view->buffer_type == VNET_ADMIN_CMD_BUFFER_DOCA ? view->result_doca_buf != NULL : view->result != NULL;
}

static bool vnet_admin_reserved_is_zero(const uint8_t *reserved, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (reserved[i] != 0)
			return false;
	}

	return true;
}

static bool vnet_admin_opcode_is_set(const uint64_t *opcodes, uint16_t opcode)
{
	uint16_t word = opcode / 64, bit = opcode % 64;

	return word < VNET_ADMIN_CMD_OPCODE_WORDS && (opcodes[word] & (1ULL << bit)) != 0;
}

static bool vnet_admin_cap_target_validate(uint16_t opcode,
					   uint16_t group_type,
					   uint64_t group_member_id,
					   struct vnet_admin_cmd_result *result)
{
	if (group_type == VIRTIO_ADMIN_GROUP_TYPE_SRIOV) {
		if (group_member_id != 0) {
			DOCA_LOG_ERR("Admin capability opcode %u with SRIOV group requires member zero", opcode);
			vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_MEMBER);
			return false;
		}
		return true;
	}
	if (group_type != VIRTIO_ADMIN_GROUP_TYPE_SELF) {
		DOCA_LOG_ERR("Admin capability opcode %u requires SELF group", opcode);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_GROUP);
		return false;
	}
	if (group_member_id != 0) {
		DOCA_LOG_ERR("Admin capability opcode %u requires member zero", opcode);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_MEMBER);
		return false;
	}

	return true;
}

static struct pci_device_config *vnet_admin_pf_get(struct vnet_pci_dev_controller *ctrl,
						   struct vnet_admin_cmd_result *result)
{
	struct vnet_pci_device *dev = atomic_load(&ctrl->virtio_device);

	if (dev == NULL || ctrl->tlp_ctx == NULL || dev->pf_index < 0 ||
	    (uint32_t)dev->pf_index >= ctrl->tlp_ctx->num_ep) {
		DOCA_LOG_ERR("Admin request cannot resolve its PF");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND);
		return NULL;
	}

	return &ctrl->tlp_ctx->devs_config[FIRST_PF_IDX(ctrl->tlp_ctx) + (uint32_t)dev->pf_index];
}

/* The caller holds pf->endpoint_lock while resolving and using the borrowed slot. */
static struct vnet_vf_slot *vnet_admin_vf_slot_get_locked(struct pci_device_config *pf,
							  uint64_t group_member_id,
							  uint16_t *slot_idx_out,
							  struct vnet_admin_cmd_result *result)
{
	struct vnet_vf_slot *slot = NULL;
	uint64_t slot_idx = 0;

	if (group_member_id != 0 && pf->sriov != NULL && pf->sriov->vf_slots != NULL) {
		slot_idx = group_member_id - 1;
		if (slot_idx < pf->sriov->total_vf) {
			slot = &pf->sriov->vf_slots[slot_idx];
			if (!atomic_load(&slot->active) || slot->vf_dev == NULL)
				slot = NULL;
		}
	}
	if (slot == NULL) {
		DOCA_LOG_ERR("Admin request targets inactive PF-local VF member %" PRIu64, group_member_id);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_MEMBER);
		return NULL;
	}

	*slot_idx_out = (uint16_t)slot_idx;
	return slot;
}

static bool vnet_admin_validate_opcode_state(struct vnet_admin_queue_state *state,
					     uint16_t opcode,
					     struct vnet_admin_cmd_result *result)
{
	bool list_use_completed, opcode_accepted;

	if (opcode == VIRTIO_ADMIN_CMD_LIST_QUERY || opcode == VIRTIO_ADMIN_CMD_LIST_USE)
		return true;

	pthread_mutex_lock(&state->state_lock);
	list_use_completed = state->list_use_completed;
	opcode_accepted = vnet_admin_opcode_is_set(state->accepted_opcodes, opcode);
	pthread_mutex_unlock(&state->state_lock);

	if (!vnet_admin_opcode_is_set(state->supported_opcodes, opcode) || !list_use_completed) {
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_OPCODE);
		return false;
	}
	if (!opcode_accepted) {
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return false;
	}

	return true;
}

static void vnet_admin_handle_list_query(const struct vnet_admin_queue_state *state,
					 const struct vnet_admin_cmd_view *view,
					 struct vnet_admin_cmd_result *result)
{
	uint64_t opcodes[VNET_ADMIN_CMD_OPCODE_WORDS] = {0};
	uint32_t write_len;
	size_t i;

	if (view->result_len < sizeof(uint64_t) || view->result_len % sizeof(uint64_t) != 0) {
		DOCA_LOG_ERR("LIST_QUERY result buffer is invalid (len=%u, min=%zu, word=%zu)",
			     view->result_len,
			     sizeof(uint64_t),
			     sizeof(uint64_t));
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	for (i = 0; i < VNET_ADMIN_CMD_OPCODE_WORDS; i++)
		opcodes[i] = htole64(state->supported_opcodes[i]);
	write_len = view->result_len < sizeof(opcodes) ? view->result_len : (uint32_t)sizeof(opcodes);
	if (!vnet_admin_cmd_view_write(view, opcodes, write_len)) {
		DOCA_LOG_ERR("Failed to write LIST_QUERY result (%u bytes)", write_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
	result->len = write_len;
}

static void vnet_admin_handle_list_use(struct vnet_admin_queue_state *state,
				       const struct vnet_admin_cmd_view *view,
				       struct vnet_admin_cmd_result *result)
{
	uint64_t requested[VNET_ADMIN_CMD_OPCODE_WORDS] = {0};
	size_t words, i;

	if (view->data_len < sizeof(uint64_t) || view->data_len % sizeof(uint64_t) != 0 ||
	    view->data_len > sizeof(requested)) {
		DOCA_LOG_ERR("LIST_USE data buffer is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (!vnet_admin_cmd_view_read(view, requested, view->data_len)) {
		DOCA_LOG_ERR("Failed to read LIST_USE data (%u bytes)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	words = view->data_len / sizeof(uint64_t);
	for (i = 0; i < words; i++) {
		requested[i] = le64toh(requested[i]);
		if ((requested[i] & ~state->supported_opcodes[i]) != 0) {
			DOCA_LOG_ERR("LIST_USE requested unsupported opcode word %zu", i);
			vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_OPCODE);
			return;
		}
	}

	pthread_mutex_lock(&state->state_lock);
	if (!state->accepting && !state->internal_active) {
		pthread_mutex_unlock(&state->state_lock);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}
	memcpy(state->accepted_opcodes, requested, sizeof(state->accepted_opcodes));
	state->list_use_completed = true;
	pthread_mutex_unlock(&state->state_lock);
	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
}

static void vnet_admin_handle_cap_id_list_query(const struct vnet_admin_cmd_view *view,
						struct vnet_admin_cmd_result *result)
{
	struct virtio_admin_cmd_query_cap_id_result cap_ids = {
		.supported_caps = {htole64(1ULL << VIRTIO_DEV_PARTS_CAP)},
	};

	if (!vnet_admin_cmd_view_has_result(view) || view->result_len < sizeof(cap_ids)) {
		DOCA_LOG_ERR("CAP_ID_LIST_QUERY result buffer is too small (len=%u)", view->result_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}
	if (!vnet_admin_cmd_view_write(view, &cap_ids, sizeof(cap_ids))) {
		DOCA_LOG_ERR("Failed to write CAP_ID_LIST_QUERY result");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
	result->len = sizeof(cap_ids);
}

static void vnet_admin_handle_device_cap_get(struct vnet_admin_queue_state *state,
					     const struct vnet_admin_cmd_view *view,
					     struct vnet_admin_cmd_result *result)
{
	struct virtio_admin_cmd_cap_get_data data = {0};
	struct virtio_dev_parts_cap cap = {0};

	if (!vnet_admin_cmd_view_has_data(view) || view->data_len != sizeof(data) ||
	    !vnet_admin_cmd_view_has_result(view) || view->result_len < sizeof(cap)) {
		DOCA_LOG_ERR("DEVICE_CAP_GET buffers are invalid (data=%u, result=%u)",
			     view->data_len,
			     view->result_len);
		vnet_admin_result_set(result,
				      !vnet_admin_cmd_view_has_result(view) || view->result_len < sizeof(cap) ?
					      VIRTIO_ADMIN_STATUS_Q_NORESOURCE :
					      VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (!vnet_admin_cmd_view_read(view, &data, sizeof(data))) {
		DOCA_LOG_ERR("Failed to read DEVICE_CAP_GET data");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (le16toh(data.id) != VIRTIO_DEV_PARTS_CAP) {
		DOCA_LOG_ERR("DEVICE_CAP_GET capability id %u is unsupported", le16toh(data.id));
		result->status = VIRTIO_ADMIN_STATUS_ENXIO;
		result->status_qualifier = VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD;
		result->len = 0;
		return;
	}
	if (!vnet_admin_reserved_is_zero(data.reserved, sizeof(data.reserved))) {
		DOCA_LOG_ERR("DEVICE_CAP_GET reserved fields are nonzero");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	cap.get_parts_resource_objects_limit = state->device_caps.get_parts_limit;
	cap.set_parts_resource_objects_limit = state->device_caps.set_parts_limit;
	if (!vnet_admin_cmd_view_write(view, &cap, sizeof(cap))) {
		DOCA_LOG_ERR("Failed to write DEVICE_CAP_GET result");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
	result->len = sizeof(cap);
}

static uint16_t vnet_admin_resource_count_locked(const struct vnet_admin_queue_state *state, uint8_t parts_type)
{
	uint16_t count = 0;
	uint32_t i;

	for (i = 0; i < VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT; i++) {
		if (state->resources[i].valid && state->resources[i].dev_parts_type == parts_type)
			count++;
	}
	return count;
}

static void vnet_admin_handle_driver_cap_set(struct vnet_admin_queue_state *state,
					     const struct vnet_admin_cmd_view *view,
					     struct vnet_admin_cmd_result *result)
{
	uint8_t payload[sizeof(struct virtio_admin_cmd_cap_set_data) + sizeof(struct virtio_dev_parts_cap)] = {0};
	struct virtio_admin_cmd_cap_set_data *data;
	struct virtio_dev_parts_cap *dev_parts;

	data = (void *)payload;
	if (view->data_len != sizeof(payload) || !vnet_admin_cmd_view_read(view, payload, sizeof(payload))) {
		DOCA_LOG_ERR("DRIVER_CAP_SET data buffer is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (le16toh(data->id) != VIRTIO_DEV_PARTS_CAP) {
		DOCA_LOG_ERR("DRIVER_CAP_SET capability id %u is unsupported", le16toh(data->id));
		result->status = VIRTIO_ADMIN_STATUS_ENXIO;
		result->status_qualifier = VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD;
		result->len = 0;
		return;
	}
	dev_parts = (void *)data->cap_specific_data;
	if (!vnet_admin_reserved_is_zero(data->reserved, sizeof(data->reserved)) ||
	    dev_parts->get_parts_resource_objects_limit > state->device_caps.get_parts_limit ||
	    dev_parts->set_parts_resource_objects_limit > state->device_caps.set_parts_limit) {
		DOCA_LOG_ERR("DRIVER_CAP_SET device-parts limits or reserved fields are invalid");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	pthread_mutex_lock(&state->state_lock);
	if (!state->accepting && !state->internal_active) {
		pthread_mutex_unlock(&state->state_lock);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}
	if (dev_parts->get_parts_resource_objects_limit <
		    vnet_admin_resource_count_locked(state, VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_GET) ||
	    dev_parts->set_parts_resource_objects_limit <
		    vnet_admin_resource_count_locked(state, VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_SET)) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("DRIVER_CAP_SET limits cannot be lower than active object counts");
		result->status = VIRTIO_ADMIN_STATUS_EBUSY;
		result->status_qualifier = VIRTIO_ADMIN_STATUS_Q_TRYAGAIN;
		result->len = 0;
		return;
	}
	state->driver_caps.get_parts_limit = dev_parts->get_parts_resource_objects_limit;
	state->driver_caps.set_parts_limit = dev_parts->set_parts_resource_objects_limit;
	state->driver_caps_set = true;
	pthread_mutex_unlock(&state->state_lock);
	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
}

static void vnet_admin_resource_error_set(struct vnet_admin_cmd_result *result, uint8_t status, uint16_t qualifier)
{
	result->status = status;
	result->status_qualifier = qualifier;
	result->len = 0;
}

static bool vnet_admin_read_is_busy(const struct vnet_vf_slot *slot, struct vnet_admin_cmd_result *result)
{
	if (!atomic_load(&slot->admin_mutating) && !vnet_recycle_in_progress(&slot->recycle))
		return false;

	vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_EBUSY, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
	return true;
}

static struct vnet_admin_resource_obj *vnet_admin_resource_find(struct vnet_admin_queue_state *state,
								uint16_t type,
								uint32_t id)
{
	uint32_t i;

	for (i = 0; i < VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT; i++) {
		if (state->resources[i].valid && state->resources[i].type == type && state->resources[i].id == id)
			return &state->resources[i];
	}

	return NULL;
}

static struct vnet_admin_resource_obj *vnet_admin_resource_free_slot_get(struct vnet_admin_queue_state *state)
{
	uint32_t i;

	for (i = 0; i < VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT; i++) {
		if (!state->resources[i].valid)
			return &state->resources[i];
	}

	return NULL;
}

static bool vnet_admin_resource_owner_has_object_locked(const struct vnet_admin_queue_state *state,
							const struct pci_device_config *pf,
							uint16_t slot_idx)
{
	uint32_t i;

	for (i = 0; i < VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT; i++) {
		if (state->resources[i].valid && state->resources[i].pf == pf &&
		    state->resources[i].slot_idx == slot_idx)
			return true;
	}
	return false;
}

static struct vnet_admin_resource_obj *vnet_admin_resource_get(struct vnet_admin_queue_state *state,
							       struct pci_device_config *pf,
							       uint16_t slot_idx,
							       uint16_t type,
							       uint32_t id,
							       uint8_t required_parts_type,
							       enum vnet_admin_resource_state operation_state,
							       struct vnet_admin_cmd_result *result)
{
	struct vnet_admin_resource_obj *obj;

	pthread_mutex_lock(&state->state_lock);
	if (!state->accepting && !state->internal_active) {
		pthread_mutex_unlock(&state->state_lock);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return NULL;
	}
	if (type != VIRTIO_RESOURCE_OBJ_DEV_PARTS) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("Resource object type %u id %u is unknown", type, id);
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_ENXIO, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return NULL;
	}

	obj = vnet_admin_resource_find(state, type, id);
	if (obj == NULL || obj->pf != pf || obj->slot_idx != slot_idx) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("Resource object type %u id %u does not belong to target VF", type, id);
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_ENXIO, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return NULL;
	}
	if (required_parts_type <= VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_SET &&
	    obj->dev_parts_type != required_parts_type) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("Resource object id %u has incompatible device-parts purpose", id);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return NULL;
	}
	if (obj->state != VNET_ADMIN_RESOURCE_IDLE) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("Resource object id %u is busy", id);
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_EBUSY, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return NULL;
	}

	obj->inflight_refs++;
	obj->state = operation_state;
	pthread_mutex_unlock(&state->state_lock);
	return obj;
}

static void vnet_admin_resource_put(struct vnet_admin_queue_state *state, struct vnet_admin_resource_obj *obj)
{
	pthread_mutex_lock(&state->state_lock);
	obj->state = VNET_ADMIN_RESOURCE_IDLE;
	obj->inflight_refs--;
	pthread_cond_broadcast(&state->state_cond);
	pthread_mutex_unlock(&state->state_lock);
}

static void vnet_admin_resource_create(struct vnet_admin_queue_state *state,
				       struct pci_device_config *pf,
				       uint16_t slot_idx,
				       uint16_t type,
				       uint32_t id,
				       uint8_t dev_parts_type,
				       struct vnet_admin_cmd_result *result)
{
	struct vnet_admin_resource_obj *obj;
	uint8_t limit;

	pthread_mutex_lock(&state->state_lock);
	if (!state->accepting && !state->internal_active) {
		pthread_mutex_unlock(&state->state_lock);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}

	limit = dev_parts_type == VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_GET ? state->driver_caps.get_parts_limit :
									   state->driver_caps.set_parts_limit;
	if (!state->driver_caps_set) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE requires negotiated device-parts capability");
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_ENOSPC, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}
	if (id >= limit) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE id %u is outside negotiated range [0, %u)", id, limit);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (vnet_admin_resource_count_locked(state, dev_parts_type) >= limit) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE reached negotiated object limit %u", limit);
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_ENOSPC, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}
	if (vnet_admin_resource_find(state, type, id) != NULL) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE duplicate type %u id %u", type, id);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (vnet_admin_resource_owner_has_object_locked(state, pf, slot_idx)) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE target VF already has an active object");
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_EBUSY, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}

	obj = vnet_admin_resource_free_slot_get(state);
	if (obj == NULL) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE has no free storage slot");
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_ENOSPC, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}

	memset(obj, 0, sizeof(*obj));
	obj->pf = pf;
	obj->slot_idx = slot_idx;
	obj->id = id;
	obj->type = type;
	obj->dev_parts_type = dev_parts_type;
	obj->state = VNET_ADMIN_RESOURCE_IDLE;
	obj->valid = true;
	pthread_mutex_unlock(&state->state_lock);

	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
}

static void vnet_admin_handle_resource_create(struct vnet_admin_queue_state *state,
					      struct pci_device_config *pf,
					      uint16_t slot_idx,
					      const struct vnet_admin_cmd_view *view,
					      struct vnet_admin_cmd_result *result)
{
	/* The wire payload is the fixed command prefix plus its device-parts flexible-array tail. */
	uint8_t payload[sizeof(struct virtio_admin_cmd_resource_obj_create_data) +
			sizeof(struct virtio_resource_obj_dev_parts)] = {0};
	struct virtio_admin_cmd_resource_obj_create_data *data;
	struct virtio_resource_obj_dev_parts *dev_parts;
	uint32_t id;
	uint16_t type;

	data = (void *)payload;
	if (view->data_len != sizeof(payload) || !vnet_admin_cmd_view_read(view, payload, sizeof(payload))) {
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE data buffer is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	dev_parts = (void *)data->resource_obj_specific_data;
	type = le16toh(data->hdr.type);
	id = le32toh(data->hdr.id);
	if (type != VIRTIO_RESOURCE_OBJ_DEV_PARTS || le64toh(data->flags) != 0 ||
	    !vnet_admin_reserved_is_zero(data->hdr.reserved, sizeof(data->hdr.reserved)) ||
	    !vnet_admin_reserved_is_zero(dev_parts->reserved, sizeof(dev_parts->reserved)) ||
	    dev_parts->type > VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_SET) {
		DOCA_LOG_ERR("RESOURCE_OBJ_CREATE fields are invalid (type=%u, id=%u)", type, id);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	vnet_admin_resource_create(state, pf, slot_idx, type, id, dev_parts->type, result);
}

static void vnet_admin_handle_resource_destroy(struct vnet_admin_queue_state *state,
					       struct pci_device_config *pf,
					       uint16_t slot_idx,
					       const struct vnet_admin_cmd_view *view,
					       struct vnet_admin_cmd_result *result)
{
	struct virtio_admin_cmd_resource_obj_cmd_hdr data = {0};
	struct vnet_admin_resource_obj *obj;
	uint32_t id;
	uint16_t type;

	if (view->data_len != sizeof(data) || !vnet_admin_cmd_view_read(view, &data, sizeof(data))) {
		DOCA_LOG_ERR("RESOURCE_OBJ_DESTROY data buffer is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	type = le16toh(data.type);
	id = le32toh(data.id);
	if (!vnet_admin_reserved_is_zero(data.reserved, sizeof(data.reserved))) {
		DOCA_LOG_ERR("RESOURCE_OBJ_DESTROY reserved fields are nonzero");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	pthread_mutex_lock(&state->state_lock);
	if (!state->accepting && !state->internal_active) {
		pthread_mutex_unlock(&state->state_lock);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}
	obj = type == VIRTIO_RESOURCE_OBJ_DEV_PARTS ? vnet_admin_resource_find(state, type, id) : NULL;
	if (obj == NULL || obj->pf != pf || obj->slot_idx != slot_idx) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_DESTROY object type %u id %u is unknown", type, id);
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_ENXIO, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (obj->inflight_refs != 0 || obj->state != VNET_ADMIN_RESOURCE_IDLE) {
		pthread_mutex_unlock(&state->state_lock);
		DOCA_LOG_ERR("RESOURCE_OBJ_DESTROY object id %u is busy", id);
		vnet_admin_resource_error_set(result, VIRTIO_ADMIN_STATUS_EBUSY, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}

	obj->state = VNET_ADMIN_RESOURCE_DESTROYING;
	vnet_admin_dev_parts_snapshot_reset(obj);
	memset(obj, 0, sizeof(*obj));
	pthread_mutex_unlock(&state->state_lock);
	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
}

static void vnet_admin_handle_metadata_get(struct vnet_admin_queue_state *state,
					   struct pci_device_config *pf,
					   uint16_t slot_idx,
					   struct vnet_vf_slot *slot,
					   const struct vnet_admin_cmd_view *view,
					   struct vnet_admin_cmd_result *result)
{
	struct virtio_admin_cmd_dev_parts_metadata_data data = {0};
	struct vnet_admin_resource_obj *obj;

	if (view->data_len != sizeof(data) || !vnet_admin_cmd_view_read(view, &data, sizeof(data)) ||
	    !vnet_admin_reserved_is_zero(data.hdr.reserved, sizeof(data.hdr.reserved)) ||
	    !vnet_admin_reserved_is_zero(data.reserved, sizeof(data.reserved))) {
		DOCA_LOG_ERR("DEV_PARTS_METADATA_GET data is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	pthread_rwlock_rdlock(&slot->admin_config_lock);
	if (vnet_admin_read_is_busy(slot, result)) {
		pthread_rwlock_unlock(&slot->admin_config_lock);
		return;
	}

	obj = vnet_admin_resource_get(state,
				      pf,
				      slot_idx,
				      le16toh(data.hdr.type),
				      le32toh(data.hdr.id),
				      VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_GET,
				      VNET_ADMIN_RESOURCE_GET_IN_PROGRESS,
				      result);
	if (obj == NULL) {
		pthread_rwlock_unlock(&slot->admin_config_lock);
		return;
	}
	vnet_admin_dev_parts_metadata_get(obj, slot, view, data.type, result);
	vnet_admin_resource_put(state, obj);
	pthread_rwlock_unlock(&slot->admin_config_lock);
}

static void vnet_admin_handle_dev_parts_get(struct vnet_admin_queue_state *state,
					    struct pci_device_config *pf,
					    uint16_t slot_idx,
					    struct vnet_vf_slot *slot,
					    const struct vnet_admin_cmd_view *view,
					    struct vnet_admin_cmd_result *result)
{
	struct virtio_admin_cmd_dev_parts_get_data *data;
	struct vnet_admin_resource_obj *obj;
	uint8_t *payload;
	uint32_t max_data_len;
	uint32_t hdr_count;

	max_data_len = sizeof(*data) + vnet_admin_dev_parts_max_header_list_size(slot->vf_dev->vqs_count);
	if (view->data_len < sizeof(*data) || view->data_len > max_data_len ||
	    (view->data_len - sizeof(*data)) % sizeof(struct virtio_dev_part_hdr) != 0) {
		DOCA_LOG_ERR("DEV_PARTS_GET data length is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	payload = calloc(1, view->data_len);
	if (payload == NULL) {
		DOCA_LOG_ERR("Failed to allocate DEV_PARTS_GET payload (%u bytes)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}
	data = (void *)payload;
	if (!vnet_admin_cmd_view_read(view, payload, view->data_len) ||
	    !vnet_admin_reserved_is_zero(data->hdr.reserved, sizeof(data->hdr.reserved)) ||
	    !vnet_admin_reserved_is_zero(data->reserved, sizeof(data->reserved)) ||
	    (data->type != VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_ALL &&
	     data->type != VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_SELECTED) ||
	    (data->type == VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_ALL && view->data_len != sizeof(*data))) {
		DOCA_LOG_ERR("DEV_PARTS_GET fields are invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		goto out;
	}
	hdr_count = (view->data_len - sizeof(*data)) / sizeof(struct virtio_dev_part_hdr);
	if (data->type == VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_SELECTED && hdr_count == 0) {
		DOCA_LOG_ERR("DEV_PARTS_GET SELECTED has no part headers");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		goto out;
	}

	pthread_rwlock_rdlock(&slot->admin_config_lock);
	if (vnet_admin_read_is_busy(slot, result)) {
		pthread_rwlock_unlock(&slot->admin_config_lock);
		goto out;
	}

	obj = vnet_admin_resource_get(state,
				      pf,
				      slot_idx,
				      le16toh(data->hdr.type),
				      le32toh(data->hdr.id),
				      VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_GET,
				      VNET_ADMIN_RESOURCE_GET_IN_PROGRESS,
				      result);
	if (obj == NULL) {
		pthread_rwlock_unlock(&slot->admin_config_lock);
		goto out;
	}
	if (data->type == VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_ALL)
		vnet_admin_dev_parts_get(obj, slot, view, result);
	else
		vnet_admin_dev_parts_get_selected(obj, slot, data->hdr_list, hdr_count, view, result);
	vnet_admin_resource_put(state, obj);
	pthread_rwlock_unlock(&slot->admin_config_lock);
out:
	free(payload);
}

static uint16_t vnet_admin_mutation_submit(struct vnet_admin_queue_state *state,
					   struct vnet_vf_slot *slot,
					   struct vnet_admin_async_cmd *cmd,
					   bool stop_requested,
					   bool internal)
{
	bool expected = false;

	pthread_mutex_lock(&state->state_lock);
	if ((!state->accepting && (!internal || !state->internal_active)) ||
	    !atomic_compare_exchange_strong(&slot->admin_mutating, &expected, true)) {
		pthread_mutex_unlock(&state->state_lock);
		return VIRTIO_ADMIN_STATUS_Q_TRYAGAIN;
	}
	if (stop_requested)
		atomic_store(&slot->admin_stop_requested, true);
	state->retained_count++;
	if (!pci_cfg_workqueue_submit_admin_cmd(cmd)) {
		state->retained_count--;
		if (stop_requested)
			atomic_store(&slot->admin_stop_requested, false);
		atomic_store(&slot->admin_mutating, false);
		pthread_mutex_unlock(&state->state_lock);
		return VIRTIO_ADMIN_STATUS_Q_NORESOURCE;
	}
	pthread_mutex_unlock(&state->state_lock);
	return VIRTIO_ADMIN_STATUS_Q_OK;
}

static void vnet_admin_async_release_operation(struct vnet_admin_async_cmd *cmd, struct vnet_vf_slot *slot)
{
	struct vnet_admin_queue_state *state = &cmd->ctrl->admin_queue;

	pthread_mutex_lock(&state->state_lock);
	if (cmd->obj != NULL) {
		cmd->obj->state = VNET_ADMIN_RESOURCE_IDLE;
		cmd->obj->inflight_refs--;
	}
	if (slot != NULL)
		atomic_store(&slot->admin_mutating, false);
	pthread_mutex_unlock(&state->state_lock);
	vnet_admin_dev_parts_restore_state_destroy(cmd->restore_state);
}

struct vnet_admin_async_completion {
	vnet_admin_cmd_complete_cb callback;
	void *context;
};

static uint16_t vnet_admin_async_opcode_get(const struct vnet_admin_async_cmd *cmd)
{
	switch (cmd->type) {
	case VNET_ADMIN_ASYNC_DEV_PARTS_SET:
		return VIRTIO_ADMIN_CMD_DEV_PARTS_SET;
	case VNET_ADMIN_ASYNC_DEV_MODE_SET:
		return VIRTIO_ADMIN_CMD_DEV_MODE_SET;
	default:
		return UINT16_MAX;
	}
}

static void vnet_admin_async_complete(struct vnet_admin_async_cmd *cmd,
				      uint8_t status,
				      uint16_t qualifier,
				      struct vnet_admin_async_completion *completion)
{
	completion->callback = cmd->complete;
	completion->context = cmd->complete_context;
	if (completion->callback == NULL) {
		DOCA_LOG_INFO("Admin VQ deferred completion: opcode=0x%04x group_type=%u group_member=%u "
			      "status=%u qualifier=%u result_len=0",
			      vnet_admin_async_opcode_get(cmd),
			      VIRTIO_ADMIN_GROUP_TYPE_SRIOV,
			      (uint32_t)cmd->slot_idx + 1U,
			      status,
			      qualifier);
		doca_devemu_virtio_admin_req_complete(cmd->req, status, qualifier, 0);
	}
}

static void vnet_admin_async_completion_invoke(const struct vnet_admin_async_completion *completion,
					       uint8_t status,
					       uint16_t qualifier)
{
	if (completion->callback != NULL)
		completion->callback(completion->context, status, qualifier, 0);
}

static void vnet_admin_async_finish(struct vnet_admin_async_cmd *cmd)
{
	struct vnet_admin_queue_state *state = &cmd->ctrl->admin_queue;

	pthread_mutex_lock(&state->state_lock);
	state->retained_count--;
	pthread_cond_broadcast(&state->state_cond);
	pthread_mutex_unlock(&state->state_lock);
	free(cmd);
}

static bool vnet_admin_async_is_valid(const struct vnet_admin_async_cmd *cmd, const struct vnet_vf_slot *slot)
{
	return cmd->generation == atomic_load(&slot->admin_operation_generation) &&
	       !vnet_recycle_in_progress(&slot->recycle) && atomic_load(&slot->active);
}

static bool vnet_admin_dev_parts_set_payload_read(const struct vnet_vf_slot *slot,
						  const struct vnet_admin_cmd_view *view,
						  struct vnet_admin_cmd_result *result,
						  uint8_t **payload)
{
	if (view->data_len <= sizeof(struct virtio_admin_cmd_resource_obj_cmd_hdr) ||
	    view->data_len - sizeof(struct virtio_admin_cmd_resource_obj_cmd_hdr) >
		    vnet_admin_dev_parts_max_payload_size(slot->vf_dev->vqs_count)) {
		DOCA_LOG_ERR("DEV_PARTS_SET data buffer is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return false;
	}
	if (view->req == NULL && view->complete == NULL) {
		DOCA_LOG_ERR("DEV_PARTS_SET has no deferred completion target");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND);
		return false;
	}
	*payload = calloc(1, view->data_len);
	if (*payload == NULL) {
		DOCA_LOG_ERR("Failed to allocate DEV_PARTS_SET payload (%u bytes)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return false;
	}
	if (!vnet_admin_cmd_view_read(view, *payload, view->data_len)) {
		DOCA_LOG_ERR("Failed to copy complete DEV_PARTS_SET payload (%u bytes)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		free(*payload);
		*payload = NULL;
		return false;
	}
	return true;
}

static bool vnet_admin_dev_parts_set_prepare(struct vnet_admin_queue_state *state,
					     struct pci_device_config *pf,
					     uint16_t slot_idx,
					     struct vnet_vf_slot *slot,
					     const uint8_t *payload,
					     uint32_t payload_len,
					     struct vnet_admin_cmd_result *result,
					     struct vnet_admin_resource_obj **obj,
					     struct vnet_admin_dev_parts_restore_state **restore_state)
{
	/* The copied payload starts with the resource-object header, followed by device-part TLVs. */
	const struct virtio_admin_cmd_resource_obj_cmd_hdr *hdr =
		(const struct virtio_admin_cmd_resource_obj_cmd_hdr *)payload;
	doca_error_t error;

	if (!vnet_admin_reserved_is_zero(hdr->reserved, sizeof(hdr->reserved))) {
		DOCA_LOG_ERR("DEV_PARTS_SET resource header reserved fields are nonzero");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return false;
	}
	*obj = vnet_admin_resource_get(state,
				       pf,
				       slot_idx,
				       le16toh(hdr->type),
				       le32toh(hdr->id),
				       VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_SET,
				       VNET_ADMIN_RESOURCE_SET_IN_PROGRESS,
				       result);
	if (*obj == NULL)
		return false;

	error = vnet_admin_dev_parts_restore_state_create(slot,
							  payload + sizeof(*hdr),
							  payload_len - sizeof(*hdr),
							  restore_state);
	if (error != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DEV_PARTS_SET payload validation failed");
		vnet_admin_result_set(result,
				      error == DOCA_ERROR_NO_MEMORY ? VIRTIO_ADMIN_STATUS_Q_NORESOURCE :
								      VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return false;
	}
	if (!atomic_load(&slot->admin_stopped)) {
		DOCA_LOG_ERR("DEV_PARTS_SET requires stopped VF mode");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return false;
	}
	return true;
}

static bool vnet_admin_dev_parts_set_submit(struct vnet_pci_dev_controller *ctrl,
					    struct pci_device_config *pf,
					    uint16_t slot_idx,
					    struct vnet_vf_slot *slot,
					    const struct vnet_admin_cmd_view *view,
					    struct vnet_admin_resource_obj *obj,
					    struct vnet_admin_dev_parts_restore_state *restore_state,
					    uint64_t generation,
					    bool internal,
					    struct vnet_admin_cmd_result *result)
{
	struct vnet_admin_async_cmd *cmd;
	uint16_t qualifier;

	cmd = calloc(1, sizeof(*cmd));
	if (cmd == NULL) {
		DOCA_LOG_ERR("Failed to allocate DEV_PARTS_SET submission");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return false;
	}
	cmd->ctrl = ctrl;
	cmd->req = view->req;
	cmd->obj = obj;
	cmd->restore_state = restore_state;
	cmd->pf = pf;
	cmd->complete = view->complete;
	cmd->complete_context = view->complete_context;
	cmd->slot_idx = slot_idx;
	cmd->generation = generation;
	cmd->type = VNET_ADMIN_ASYNC_DEV_PARTS_SET;
	qualifier = vnet_admin_mutation_submit(&ctrl->admin_queue, slot, cmd, false, internal);
	if (qualifier != VIRTIO_ADMIN_STATUS_Q_OK) {
		DOCA_LOG_ERR("DEV_PARTS_SET target VF is busy or work cannot be queued");
		vnet_admin_resource_error_set(result,
					      qualifier == VIRTIO_ADMIN_STATUS_Q_NORESOURCE ?
						      VIRTIO_ADMIN_STATUS_ENOSPC :
						      VIRTIO_ADMIN_STATUS_EBUSY,
					      qualifier);
		free(cmd);
		return false;
	}

	result->deferred = true;
	return true;
}

static void vnet_admin_handle_dev_parts_set(struct vnet_pci_dev_controller *ctrl,
					    struct pci_device_config *pf,
					    uint16_t slot_idx,
					    struct vnet_vf_slot *slot,
					    const struct vnet_admin_cmd_view *view,
					    bool internal,
					    struct vnet_admin_cmd_result *result)
{
	struct vnet_admin_dev_parts_restore_state *restore_state = NULL;
	struct vnet_admin_queue_state *state = &ctrl->admin_queue;
	struct vnet_admin_resource_obj *obj = NULL;
	uint8_t *payload = NULL;
	uint64_t generation;

	/* Command-specific data is [resource-object header][device-part header][value]... */
	if (!vnet_admin_dev_parts_set_payload_read(slot, view, result, &payload))
		return;
	pthread_rwlock_wrlock(&slot->admin_config_lock);
	generation = atomic_load(&slot->admin_operation_generation);
	if (!vnet_admin_dev_parts_set_prepare(state,
					      pf,
					      slot_idx,
					      slot,
					      payload,
					      view->data_len,
					      result,
					      &obj,
					      &restore_state))
		goto out;
	if (vnet_admin_dev_parts_set_submit(ctrl,
					    pf,
					    slot_idx,
					    slot,
					    view,
					    obj,
					    restore_state,
					    generation,
					    internal,
					    result)) {
		obj = NULL;
		restore_state = NULL;
	}
out:
	pthread_rwlock_unlock(&slot->admin_config_lock);
	if (obj != NULL)
		vnet_admin_resource_put(state, obj);
	vnet_admin_dev_parts_restore_state_destroy(restore_state);
	free(payload);
}

static void vnet_admin_handle_dev_mode_set(struct vnet_pci_dev_controller *ctrl,
					   struct pci_device_config *pf,
					   uint16_t slot_idx,
					   struct vnet_vf_slot *slot,
					   const struct vnet_admin_cmd_view *view,
					   bool internal,
					   struct vnet_admin_cmd_result *result)
{
	struct virtio_admin_cmd_dev_mode_set_data data = {0};
	struct vnet_admin_async_cmd *cmd;
	uint64_t generation;
	uint16_t qualifier;

	if (view->data_len != sizeof(data) || !vnet_admin_cmd_view_read(view, &data, sizeof(data)) ||
	    (data.flags & ~(1U << VIRTIO_ADMIN_CMD_DEV_MODE_F_STOPPED)) != 0) {
		DOCA_LOG_ERR("DEV_MODE_SET data is invalid (len=%u)", view->data_len);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	if (view->req == NULL && view->complete == NULL) {
		DOCA_LOG_ERR("DEV_MODE_SET has no deferred completion target");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND);
		return;
	}

	cmd = calloc(1, sizeof(*cmd));
	if (cmd == NULL) {
		DOCA_LOG_ERR("Failed to allocate DEV_MODE_SET submission");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}
	cmd->ctrl = ctrl;
	cmd->req = view->req;
	cmd->pf = pf;
	cmd->complete = view->complete;
	cmd->complete_context = view->complete_context;
	cmd->slot_idx = slot_idx;
	cmd->type = VNET_ADMIN_ASYNC_DEV_MODE_SET;
	cmd->stopped = (data.flags & (1U << VIRTIO_ADMIN_CMD_DEV_MODE_F_STOPPED)) != 0;

	pthread_rwlock_wrlock(&slot->admin_config_lock);
	generation = atomic_load(&slot->admin_operation_generation);
	cmd->generation = generation;

	qualifier = vnet_admin_mutation_submit(&ctrl->admin_queue, slot, cmd, cmd->stopped, internal);
	pthread_rwlock_unlock(&slot->admin_config_lock);

	if (qualifier != VIRTIO_ADMIN_STATUS_Q_OK) {
		DOCA_LOG_ERR("DEV_MODE_SET target VF is busy or work cannot be queued");
		vnet_admin_resource_error_set(result,
					      qualifier == VIRTIO_ADMIN_STATUS_Q_NORESOURCE ?
						      VIRTIO_ADMIN_STATUS_ENOSPC :
						      VIRTIO_ADMIN_STATUS_EBUSY,
					      qualifier);
		free(cmd);
		return;
	}
	result->deferred = true;
}

static doca_error_t vnet_admin_async_dev_parts_set_execute(struct vnet_admin_async_cmd *cmd,
							   struct pci_device_config *pf,
							   struct vnet_vf_slot *slot)
{
	doca_error_t result;
	bool rebuild;

	if (!atomic_load(&slot->admin_stopped))
		return DOCA_ERROR_BAD_STATE;
	result = vnet_admin_dev_parts_restore_state_apply(cmd->restore_state, slot, cmd->generation, &rebuild);
	if (result == DOCA_SUCCESS && !vnet_admin_async_is_valid(cmd, slot))
		result = DOCA_ERROR_IN_PROGRESS;
	if (result == DOCA_SUCCESS && rebuild)
		result = vnet_admin_vf_rebuild_stopped(cmd->ctrl->tlp_ctx, pf, slot, cmd->generation);
	if (result == DOCA_SUCCESS && !vnet_admin_async_is_valid(cmd, slot))
		result = DOCA_ERROR_IN_PROGRESS;
	if (result == DOCA_SUCCESS && rebuild)
		atomic_store(&slot->admin_rebuild_required, false);
	return result;
}

static doca_error_t vnet_admin_async_dev_mode_stop_execute(struct vnet_admin_async_cmd *cmd, struct vnet_vf_slot *slot)
{
	doca_error_t result;

	if (!vnet_admin_async_is_valid(cmd, slot)) {
		atomic_store(&slot->admin_stop_requested, false);
		return DOCA_ERROR_IN_PROGRESS;
	}
	result = vnet_admin_vf_stop_datapath(slot, cmd->ctrl->worker_pe);
	if (result == DOCA_SUCCESS && !vnet_admin_async_is_valid(cmd, slot))
		result = DOCA_ERROR_IN_PROGRESS;
	if (result == DOCA_SUCCESS)
		atomic_store(&slot->admin_stopped, true);
	else
		atomic_store(&slot->admin_stop_requested, false);
	return result;
}

static doca_error_t vnet_admin_async_dev_mode_start_execute(struct vnet_admin_async_cmd *cmd, struct vnet_vf_slot *slot)
{
	doca_error_t result;

	if (!atomic_load(&slot->admin_stopped) && atomic_load(&slot->engine_enabled))
		return DOCA_SUCCESS;
	if ((slot->vf_dev->pci_cfg.device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK) == 0) {
		/* VFIO initializes migration mode before the guest driver is ready. */
		DOCA_LOG_INFO("SR-IOV: VF[vf_index=%u] RUNNING accepted before DRIVER_OK; datapath start deferred",
			      slot->vf_index);
		atomic_store(&slot->admin_stop_requested, false);
		atomic_store(&slot->admin_stopped, false);
		return DOCA_SUCCESS;
	}

	result = vnet_pci_dev_vf_start_and_enable(slot);
	if (result != DOCA_SUCCESS)
		return result;
	if (!vnet_admin_async_is_valid(cmd, slot)) {
		(void)vnet_admin_vf_stop_datapath(slot, cmd->ctrl->worker_pe);
		atomic_store(&slot->admin_stopped, true);
		return DOCA_ERROR_IN_PROGRESS;
	}
	atomic_store(&slot->admin_stop_requested, false);
	atomic_store(&slot->admin_stopped, false);
	return DOCA_SUCCESS;
}

static doca_error_t vnet_admin_async_dev_mode_set_execute(struct vnet_admin_async_cmd *cmd, struct vnet_vf_slot *slot)
{
	return cmd->stopped ? vnet_admin_async_dev_mode_stop_execute(cmd, slot) :
			      vnet_admin_async_dev_mode_start_execute(cmd, slot);
}

static void vnet_admin_async_status_get(doca_error_t error, uint8_t *status, uint16_t *qualifier)
{
	if (error == DOCA_SUCCESS) {
		*status = VIRTIO_ADMIN_STATUS_OK;
		*qualifier = VIRTIO_ADMIN_STATUS_Q_OK;
	} else if (error == DOCA_ERROR_NO_MEMORY) {
		*status = VIRTIO_ADMIN_STATUS_ENOMEM;
		*qualifier = VIRTIO_ADMIN_STATUS_Q_NORESOURCE;
	} else {
		*status = VIRTIO_ADMIN_STATUS_EAGAIN;
		*qualifier = VIRTIO_ADMIN_STATUS_Q_TRYAGAIN;
	}
}

doca_error_t vnet_admin_queue_async_execute(struct vnet_admin_async_cmd *cmd)
{
	struct vnet_admin_async_completion completion = {0};
	struct vnet_vf_slot *slot = NULL;
	struct pci_device_config *pf;
	doca_error_t error;
	uint16_t qualifier;
	uint8_t status;

	if (cmd == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	pf = cmd->pf;
	if (pf == NULL) {
		status = VIRTIO_ADMIN_STATUS_EAGAIN;
		qualifier = VIRTIO_ADMIN_STATUS_Q_TRYAGAIN;
		vnet_admin_async_complete(cmd, status, qualifier, &completion);
		vnet_admin_async_release_operation(cmd, NULL);
		vnet_admin_async_completion_invoke(&completion, status, qualifier);
		vnet_admin_async_finish(cmd);
		return DOCA_SUCCESS;
	}

	pthread_rwlock_rdlock(&pf->endpoint_lock);
	if (pf->sriov != NULL && pf->sriov->vf_slots != NULL && cmd->slot_idx < pf->sriov->total_vf)
		slot = &pf->sriov->vf_slots[cmd->slot_idx];
	if (slot == NULL || slot->vf_dev == NULL || !vnet_admin_async_is_valid(cmd, slot)) {
		if (slot != NULL && cmd->type == VNET_ADMIN_ASYNC_DEV_MODE_SET && cmd->stopped)
			atomic_store(&slot->admin_stop_requested, false);
		status = VIRTIO_ADMIN_STATUS_EAGAIN;
		qualifier = VIRTIO_ADMIN_STATUS_Q_TRYAGAIN;
		vnet_admin_async_complete(cmd, status, qualifier, &completion);
		vnet_admin_async_release_operation(cmd, slot);
		pthread_rwlock_unlock(&pf->endpoint_lock);
		vnet_admin_async_completion_invoke(&completion, status, qualifier);
		vnet_admin_async_finish(cmd);
		return DOCA_SUCCESS;
	}
	pthread_rwlock_wrlock(&slot->admin_config_lock);

	switch (cmd->type) {
	case VNET_ADMIN_ASYNC_DEV_PARTS_SET:
		error = vnet_admin_async_dev_parts_set_execute(cmd, pf, slot);
		break;
	case VNET_ADMIN_ASYNC_DEV_MODE_SET:
		error = vnet_admin_async_dev_mode_set_execute(cmd, slot);
		break;
	default:
		error = DOCA_ERROR_INVALID_VALUE;
		break;
	}

	if (error == DOCA_SUCCESS && !vnet_admin_async_is_valid(cmd, slot)) {
		if (cmd->type == VNET_ADMIN_ASYNC_DEV_MODE_SET && !cmd->stopped) {
			(void)vnet_admin_vf_stop_datapath(slot, cmd->ctrl->worker_pe);
			atomic_store(&slot->admin_stopped, true);
		}
		error = DOCA_ERROR_IN_PROGRESS;
	}
	vnet_admin_async_status_get(error, &status, &qualifier);
	vnet_admin_async_complete(cmd, status, qualifier, &completion);
	vnet_admin_async_release_operation(cmd, slot);
	pthread_rwlock_unlock(&slot->admin_config_lock);
	pthread_rwlock_unlock(&pf->endpoint_lock);
	vnet_admin_async_completion_invoke(&completion, status, qualifier);
	vnet_admin_async_finish(cmd);
	return DOCA_SUCCESS;
}

void vnet_admin_queue_async_cancel(struct vnet_admin_async_cmd *cmd)
{
	struct vnet_admin_async_completion completion = {0};
	struct vnet_vf_slot *slot = NULL;
	struct pci_device_config *pf;

	if (cmd == NULL)
		return;
	pf = cmd->pf;
	if (pf != NULL) {
		pthread_rwlock_rdlock(&pf->endpoint_lock);
		if (pf->sriov != NULL && pf->sriov->vf_slots != NULL && cmd->slot_idx < pf->sriov->total_vf)
			slot = &pf->sriov->vf_slots[cmd->slot_idx];
		if (slot != NULL && cmd->type == VNET_ADMIN_ASYNC_DEV_MODE_SET && cmd->stopped)
			atomic_store(&slot->admin_stop_requested, false);
	}
	vnet_admin_async_complete(cmd, VIRTIO_ADMIN_STATUS_EAGAIN, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN, &completion);
	vnet_admin_async_release_operation(cmd, slot);
	if (pf != NULL)
		pthread_rwlock_unlock(&pf->endpoint_lock);
	vnet_admin_async_completion_invoke(&completion, VIRTIO_ADMIN_STATUS_EAGAIN, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
	vnet_admin_async_finish(cmd);
}

bool vnet_admin_queue_async_is_internal(const struct vnet_admin_async_cmd *cmd)
{
	return cmd != NULL && cmd->complete != NULL;
}

uint8_t vnet_admin_status_from_qualifier(uint16_t qualifier)
{
	switch (qualifier) {
	case VIRTIO_ADMIN_STATUS_Q_OK:
		return VIRTIO_ADMIN_STATUS_OK;
	case VIRTIO_ADMIN_STATUS_Q_TRYAGAIN:
		return VIRTIO_ADMIN_STATUS_EAGAIN;
	case VIRTIO_ADMIN_STATUS_Q_NORESOURCE:
		return VIRTIO_ADMIN_STATUS_ENOMEM;
	case VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND:
	case VIRTIO_ADMIN_STATUS_Q_INVALID_OPCODE:
	case VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD:
	case VIRTIO_ADMIN_STATUS_Q_INVALID_GROUP:
	case VIRTIO_ADMIN_STATUS_Q_INVALID_MEMBER:
	default:
		return VIRTIO_ADMIN_STATUS_EINVAL;
	}
}

doca_error_t vnet_admin_queue_init(struct vnet_admin_queue_state *state)
{
	memset(state, 0, sizeof(*state));
	if (pthread_mutex_init(&state->state_lock, NULL) != 0)
		return DOCA_ERROR_OPERATING_SYSTEM;
	if (pthread_cond_init(&state->state_cond, NULL) != 0) {
		pthread_mutex_destroy(&state->state_lock);
		return DOCA_ERROR_OPERATING_SYSTEM;
	}
	state->initialized = true;
	state->accepting = true;
	state->supported_opcodes[0] = VNET_ADMIN_SUPPORTED_OPCODES;
	state->device_caps.get_parts_limit = VNET_ADMIN_DEV_PARTS_CAP_LIMIT;
	state->device_caps.set_parts_limit = VNET_ADMIN_DEV_PARTS_CAP_LIMIT;
	return DOCA_SUCCESS;
}

void vnet_admin_queue_resume(struct vnet_admin_queue_state *state)
{
	pthread_mutex_lock(&state->state_lock);
	if (state->internal_active)
		state->internal_restore_admission = true;
	else
		state->accepting = true;
	pthread_mutex_unlock(&state->state_lock);
}

void vnet_admin_queue_prepare_cleanup(struct vnet_pci_dev_controller *ctrl)
{
	struct vnet_pci_device *dev;
	struct pci_device_config *pf;
	uint16_t i;

	vnet_admin_queue_stop_admission(&ctrl->admin_queue);
	dev = atomic_load(&ctrl->virtio_device);
	if (dev == NULL || ctrl->tlp_ctx == NULL || dev->pf_index < 0 ||
	    (uint32_t)dev->pf_index >= ctrl->tlp_ctx->num_ep)
		return;

	pf = &ctrl->tlp_ctx->devs_config[FIRST_PF_IDX(ctrl->tlp_ctx) + (uint32_t)dev->pf_index];
	pthread_rwlock_rdlock(&pf->endpoint_lock);
	if (pf->sriov != NULL && pf->sriov->vf_slots != NULL) {
		for (i = 0; i < pf->sriov->total_vf; i++)
			atomic_fetch_add(&pf->sriov->vf_slots[i].admin_operation_generation, 1);
	}
	pthread_rwlock_unlock(&pf->endpoint_lock);
}

doca_error_t vnet_admin_queue_prepare_endpoint_destroy(struct tlp_context *tlp_ctx, uint32_t pf_index)
{
	struct vnet_admin_queue_state *state;

	if (tlp_ctx == NULL || pf_index >= tlp_ctx->num_ep)
		return DOCA_SUCCESS;
	state = &tlp_ctx->vnet_controller[pf_index].admin_queue;
	vnet_admin_queue_stop_admission(state);
	if (pci_cfg_workqueue_is_worker_thread()) {
		if (vnet_admin_queue_has_retained(state)) {
			DOCA_LOG_WARN("Deferring endpoint teardown until in-flight admin work drains");
			return DOCA_ERROR_AGAIN;
		}
	} else {
		vnet_admin_queue_quiesce(state);
	}
	return DOCA_SUCCESS;
}

void vnet_admin_queue_stop_admission(struct vnet_admin_queue_state *state)
{
	pthread_mutex_lock(&state->state_lock);
	state->accepting = false;
	state->internal_restore_admission = false;
	pthread_mutex_unlock(&state->state_lock);
}

bool vnet_admin_queue_begin_internal(struct vnet_admin_queue_state *state)
{
	bool started = false;

	pthread_mutex_lock(&state->state_lock);
	if (state->accepting && !state->internal_active && state->external_inflight == 0 &&
	    state->retained_count == 0) {
		state->accepting = false;
		state->internal_active = true;
		state->internal_restore_admission = true;
		started = true;
	}
	pthread_mutex_unlock(&state->state_lock);
	return started;
}

void vnet_admin_queue_end_internal(struct vnet_admin_queue_state *state)
{
	pthread_mutex_lock(&state->state_lock);
	if (state->internal_active) {
		state->internal_active = false;
		state->accepting = state->internal_restore_admission;
		state->internal_restore_admission = false;
	}
	pthread_mutex_unlock(&state->state_lock);
}

bool vnet_admin_queue_has_retained(struct vnet_admin_queue_state *state)
{
	bool retained;

	pthread_mutex_lock(&state->state_lock);
	retained = state->retained_count != 0 || state->external_inflight != 0;
	pthread_mutex_unlock(&state->state_lock);
	return retained;
}

void vnet_admin_queue_quiesce(struct vnet_admin_queue_state *state)
{
	pthread_mutex_lock(&state->state_lock);
	state->accepting = false;
	state->internal_restore_admission = false;
	while (state->retained_count != 0 || state->external_inflight != 0)
		pthread_cond_wait(&state->state_cond, &state->state_lock);
	pthread_mutex_unlock(&state->state_lock);
}

void vnet_admin_queue_reset(struct vnet_admin_queue_state *state)
{
	bool has_inflight;
	uint32_t i;

	vnet_admin_queue_quiesce(state);
	pthread_mutex_lock(&state->state_lock);
	do {
		has_inflight = false;
		for (i = 0; i < VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT; i++)
			has_inflight |= state->resources[i].inflight_refs != 0;
		if (has_inflight)
			pthread_cond_wait(&state->state_cond, &state->state_lock);
	} while (has_inflight);
	for (i = 0; i < VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT; i++) {
		if (!state->resources[i].valid)
			continue;
		vnet_admin_dev_parts_snapshot_reset(&state->resources[i]);
	}
	memset(state->resources, 0, sizeof(state->resources));
	memset(state->accepted_opcodes, 0, sizeof(state->accepted_opcodes));
	state->list_use_completed = false;
	memset(&state->driver_caps, 0, sizeof(state->driver_caps));
	state->driver_caps_set = false;
	state->external_inflight = 0;
	state->internal_active = false;
	state->internal_restore_admission = false;
	pthread_mutex_unlock(&state->state_lock);
}

void vnet_admin_queue_fini(struct vnet_admin_queue_state *state)
{
	if (!state->initialized)
		return;
	vnet_admin_queue_reset(state);
	state->initialized = false;
	pthread_cond_destroy(&state->state_cond);
	pthread_mutex_destroy(&state->state_lock);
}

struct vnet_admin_dispatch_target {
	struct pci_device_config *pf;
	struct vnet_vf_slot *vf_slot;
	uint16_t slot_idx;
	bool endpoint_lock_held;
};

static bool vnet_admin_vf_target_resolve(struct vnet_pci_dev_controller *ctrl,
					 uint16_t opcode,
					 uint16_t group_type,
					 uint64_t group_member_id,
					 struct vnet_admin_cmd_result *result,
					 struct vnet_admin_dispatch_target *target)
{
	if (group_type != VIRTIO_ADMIN_GROUP_TYPE_SRIOV) {
		DOCA_LOG_ERR("Admin VF opcode %u requires SRIOV group", opcode);
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_GROUP);
		return false;
	}

	target->pf = vnet_admin_pf_get(ctrl, result);
	if (target->pf == NULL)
		return false;

	pthread_rwlock_rdlock(&target->pf->endpoint_lock);
	target->endpoint_lock_held = true;
	target->vf_slot = vnet_admin_vf_slot_get_locked(target->pf, group_member_id, &target->slot_idx, result);
	return target->vf_slot != NULL;
}

static bool vnet_admin_dispatch_target_resolve(struct vnet_pci_dev_controller *ctrl,
					       uint16_t opcode,
					       uint16_t group_type,
					       uint64_t group_member_id,
					       struct vnet_admin_cmd_result *result,
					       struct vnet_admin_dispatch_target *target)
{
	switch (opcode) {
	case VIRTIO_ADMIN_CMD_LIST_QUERY:
	case VIRTIO_ADMIN_CMD_LIST_USE:
		return vnet_admin_list_target_validate(opcode, group_member_id, result);
	case VIRTIO_ADMIN_CMD_CAP_ID_LIST_QUERY:
	case VIRTIO_ADMIN_CMD_DEVICE_CAP_GET:
	case VIRTIO_ADMIN_CMD_DRIVER_CAP_SET:
		return vnet_admin_cap_target_validate(opcode, group_type, group_member_id, result);
	case VIRTIO_ADMIN_CMD_RESOURCE_OBJ_CREATE:
	case VIRTIO_ADMIN_CMD_RESOURCE_OBJ_DESTROY:
	case VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_GET:
	case VIRTIO_ADMIN_CMD_DEV_PARTS_GET:
	case VIRTIO_ADMIN_CMD_DEV_PARTS_SET:
	case VIRTIO_ADMIN_CMD_DEV_MODE_SET:
		return vnet_admin_vf_target_resolve(ctrl, opcode, group_type, group_member_id, result, target);
	default:
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_OPCODE);
		return false;
	}
}

static void vnet_admin_command_dispatch(struct vnet_pci_dev_controller *ctrl,
					const struct vnet_admin_cmd_view *view,
					uint16_t opcode,
					bool internal,
					const struct vnet_admin_dispatch_target *target,
					struct vnet_admin_cmd_result *result)
{
	switch (opcode) {
	case VIRTIO_ADMIN_CMD_LIST_QUERY:
		vnet_admin_handle_list_query(&ctrl->admin_queue, view, result);
		break;
	case VIRTIO_ADMIN_CMD_LIST_USE:
		vnet_admin_handle_list_use(&ctrl->admin_queue, view, result);
		break;
	case VIRTIO_ADMIN_CMD_CAP_ID_LIST_QUERY:
		vnet_admin_handle_cap_id_list_query(view, result);
		break;
	case VIRTIO_ADMIN_CMD_DEVICE_CAP_GET:
		vnet_admin_handle_device_cap_get(&ctrl->admin_queue, view, result);
		break;
	case VIRTIO_ADMIN_CMD_DRIVER_CAP_SET:
		vnet_admin_handle_driver_cap_set(&ctrl->admin_queue, view, result);
		break;
	case VIRTIO_ADMIN_CMD_RESOURCE_OBJ_CREATE:
		vnet_admin_handle_resource_create(&ctrl->admin_queue, target->pf, target->slot_idx, view, result);
		break;
	case VIRTIO_ADMIN_CMD_RESOURCE_OBJ_DESTROY:
		vnet_admin_handle_resource_destroy(&ctrl->admin_queue, target->pf, target->slot_idx, view, result);
		break;
	case VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_GET:
		vnet_admin_handle_metadata_get(&ctrl->admin_queue,
					       target->pf,
					       target->slot_idx,
					       target->vf_slot,
					       view,
					       result);
		break;
	case VIRTIO_ADMIN_CMD_DEV_PARTS_GET:
		vnet_admin_handle_dev_parts_get(&ctrl->admin_queue,
						target->pf,
						target->slot_idx,
						target->vf_slot,
						view,
						result);
		break;
	case VIRTIO_ADMIN_CMD_DEV_PARTS_SET:
		vnet_admin_handle_dev_parts_set(ctrl,
						target->pf,
						target->slot_idx,
						target->vf_slot,
						view,
						internal,
						result);
		break;
	case VIRTIO_ADMIN_CMD_DEV_MODE_SET:
		vnet_admin_handle_dev_mode_set(ctrl,
					       target->pf,
					       target->slot_idx,
					       target->vf_slot,
					       view,
					       internal,
					       result);
		break;
	default:
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_OPCODE);
		break;
	}
}

static void vnet_admin_queue_dispatch_common(struct vnet_pci_dev_controller *ctrl,
					     const struct vnet_admin_cmd_view *view,
					     uint16_t opcode,
					     uint16_t group_type,
					     uint64_t group_member_id,
					     bool internal,
					     struct vnet_admin_cmd_result *result)
{
	struct vnet_admin_dispatch_target target = {0};
	bool accepting;

	vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
	if (view == NULL) {
		DOCA_LOG_ERR("Admin request missing command buffer view");
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND);
		return;
	}
	if (ctrl == NULL) {
		DOCA_LOG_ERR("Admin request missing controller context");
		if (opcode == VIRTIO_ADMIN_CMD_LIST_QUERY || opcode == VIRTIO_ADMIN_CMD_LIST_USE) {
			result->status = VNET_ADMIN_STATUS_ERR;
			result->status_qualifier = VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND;
		} else {
			vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND);
		}
		return;
	}
	pthread_mutex_lock(&ctrl->admin_queue.state_lock);
	accepting = !internal || ctrl->admin_queue.internal_active;
	pthread_mutex_unlock(&ctrl->admin_queue.state_lock);
	if (!accepting) {
		vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}
	if (!vnet_admin_dispatch_target_resolve(ctrl, opcode, group_type, group_member_id, result, &target) ||
	    !vnet_admin_validate_opcode_state(&ctrl->admin_queue, opcode, result))
		goto out;

	vnet_admin_command_dispatch(ctrl, view, opcode, internal, &target, result);
out:
	if (target.endpoint_lock_held)
		pthread_rwlock_unlock(&target.pf->endpoint_lock);
}

void vnet_admin_queue_dispatch_view(struct vnet_pci_dev_controller *ctrl,
				    const struct vnet_admin_cmd_view *view,
				    uint16_t opcode,
				    uint16_t group_type,
				    uint64_t group_member_id,
				    struct vnet_admin_cmd_result *result)
{
	bool admitted = false;

	if (ctrl != NULL) {
		pthread_mutex_lock(&ctrl->admin_queue.state_lock);
		if (ctrl->admin_queue.accepting && !ctrl->admin_queue.internal_active) {
			ctrl->admin_queue.external_inflight++;
			admitted = true;
		}
		pthread_mutex_unlock(&ctrl->admin_queue.state_lock);
		if (!admitted) {
			vnet_admin_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
			return;
		}
	}

	vnet_admin_queue_dispatch_common(ctrl, view, opcode, group_type, group_member_id, false, result);

	if (ctrl != NULL) {
		pthread_mutex_lock(&ctrl->admin_queue.state_lock);
		ctrl->admin_queue.external_inflight--;
		pthread_cond_broadcast(&ctrl->admin_queue.state_cond);
		pthread_mutex_unlock(&ctrl->admin_queue.state_lock);
	}
}

void vnet_admin_queue_dispatch_internal(struct vnet_pci_dev_controller *ctrl,
					const struct vnet_admin_cmd_view *view,
					uint16_t opcode,
					uint16_t group_type,
					uint64_t group_member_id,
					struct vnet_admin_cmd_result *result)
{
	vnet_admin_queue_dispatch_common(ctrl, view, opcode, group_type, group_member_id, true, result);
}

void vnet_admin_queue_dispatch(struct vnet_pci_dev_controller *ctrl,
			       struct doca_devemu_virtio_admin_req *req,
			       uint16_t opcode,
			       uint16_t group_type,
			       uint64_t group_member_id,
			       struct vnet_admin_cmd_result *result)
{
	struct vnet_admin_cmd_view view = {
		.req = req,
	};

	if (ctrl != NULL) {
		view.data_doca_buf = doca_devemu_virtio_admin_req_get_cmd_specific_data(req);
		view.result_doca_buf = doca_devemu_virtio_admin_req_get_cmd_specific_result(req);
		view.data_len = doca_devemu_virtio_admin_req_get_cmd_specific_data_len(req);
		view.result_len = doca_devemu_virtio_admin_req_get_cmd_specific_result_len(req);
	}
	vnet_admin_queue_dispatch_view(ctrl, &view, opcode, group_type, group_member_id, result);
}
