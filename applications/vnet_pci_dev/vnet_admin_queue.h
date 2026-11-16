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

#ifndef VNET_ADMIN_QUEUE_H_
#define VNET_ADMIN_QUEUE_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#include <doca_error.h>

struct doca_buf;
struct doca_devemu_virtio_admin_req;
struct pci_device_config;
struct tlp_context;
struct vnet_admin_async_cmd;
struct vnet_admin_dev_parts_profile;
struct vnet_pci_dev_controller;
struct vnet_vf_slot;

enum vnet_admin_resource_state {
	VNET_ADMIN_RESOURCE_IDLE,
	VNET_ADMIN_RESOURCE_GET_IN_PROGRESS,
	VNET_ADMIN_RESOURCE_SET_IN_PROGRESS,
	VNET_ADMIN_RESOURCE_DESTROYING,
};

struct vnet_admin_resource_obj {
	/* The queue owns the profile/cache; PF and slot identity are borrowed and stable. */
	struct vnet_admin_dev_parts_profile *profile;
	struct pci_device_config *pf;
	/* A cache is a point-in-time [header][value] snapshot matching profile generation. */
	uint8_t *cache;
	uint64_t cache_generation;
	uint32_t cache_len;
	/* Object IDs are opaque protocol values and never index resources[]. */
	uint32_t id;
	uint32_t inflight_refs;
	uint16_t slot_idx;
	uint16_t type;
	uint8_t dev_parts_type;
	enum vnet_admin_resource_state state;
	bool valid;
};

#define VNET_ADMIN_CMD_OPCODE_WORDS 64
/* Architecture reserves resource IDs 0..255; the VirtIO capability field advertises at most 255. */
#define VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT 256U
#define VNET_ADMIN_DEV_PARTS_CAP_LIMIT UINT8_MAX
/* Value 1 is retained for LIST command compatibility; it is not a VirtIO 1.4 status. */
#define VNET_ADMIN_STATUS_ERR 1

struct vnet_admin_dev_parts_caps {
	uint8_t get_parts_limit;
	uint8_t set_parts_limit;
};

struct vnet_admin_queue_state {
	/* Immutable after initialization. */
	uint64_t supported_opcodes[VNET_ADMIN_CMD_OPCODE_WORDS];
	struct vnet_admin_dev_parts_caps device_caps;
	/* state_lock protects all negotiation state and the queue-owned registry. */
	uint64_t accepted_opcodes[VNET_ADMIN_CMD_OPCODE_WORDS];
	/* resources[] is the sole owner and source of truth for resource objects. */
	struct vnet_admin_resource_obj resources[VNET_ADMIN_DEV_PARTS_OBJECT_LIMIT];
	struct vnet_admin_dev_parts_caps driver_caps;
	pthread_mutex_t state_lock;
	pthread_cond_t state_cond;
	/* Retained work and external dispatches pin teardown; internal admission is exclusive. */
	uint32_t retained_count;
	uint32_t external_inflight;
	bool list_use_completed;
	bool driver_caps_set;
	bool accepting;
	bool initialized;
	bool internal_active;
	bool internal_restore_admission;
};

struct vnet_admin_cmd_result {
	uint32_t len;
	uint16_t status_qualifier;
	uint8_t status;
	bool deferred;
};

/* Deferred callbacks run outside VF locks after command state is released. */
typedef void (*vnet_admin_cmd_complete_cb)(void *context, uint8_t status, uint16_t qualifier, uint32_t len);

enum vnet_admin_cmd_buffer_type {
	VNET_ADMIN_CMD_BUFFER_DOCA,
	VNET_ADMIN_CMD_BUFFER_PLAIN,
};

struct vnet_admin_cmd_view {
	/* Buffers are borrowed for dispatch; deferred commands retain only completion fields. */
	struct doca_devemu_virtio_admin_req *req;
	struct doca_buf *data_doca_buf;
	struct doca_buf *result_doca_buf;
	const void *data;
	void *result;
	vnet_admin_cmd_complete_cb complete;
	void *complete_context;
	uint32_t data_len;
	uint32_t result_len;
	enum vnet_admin_cmd_buffer_type buffer_type;
};

uint8_t vnet_admin_status_from_qualifier(uint16_t qualifier);
bool vnet_admin_buf_list_copy(struct doca_buf *buf, void *data, uint32_t len, bool to_buf);
void vnet_admin_cmd_view_init_plain(struct vnet_admin_cmd_view *view,
				    const void *data,
				    uint32_t data_len,
				    void *result,
				    uint32_t result_len,
				    vnet_admin_cmd_complete_cb complete,
				    void *complete_context);
bool vnet_admin_cmd_view_read(const struct vnet_admin_cmd_view *view, void *data, uint32_t len);
bool vnet_admin_cmd_view_write(const struct vnet_admin_cmd_view *view, const void *data, uint32_t len);
bool vnet_admin_cmd_view_has_data(const struct vnet_admin_cmd_view *view);
bool vnet_admin_cmd_view_has_result(const struct vnet_admin_cmd_view *view);
doca_error_t vnet_admin_queue_init(struct vnet_admin_queue_state *state);
void vnet_admin_queue_resume(struct vnet_admin_queue_state *state);
void vnet_admin_queue_prepare_cleanup(struct vnet_pci_dev_controller *ctrl);
doca_error_t vnet_admin_queue_prepare_endpoint_destroy(struct tlp_context *tlp_ctx, uint32_t pf_index);
void vnet_admin_queue_stop_admission(struct vnet_admin_queue_state *state);
bool vnet_admin_queue_begin_internal(struct vnet_admin_queue_state *state);
void vnet_admin_queue_end_internal(struct vnet_admin_queue_state *state);
bool vnet_admin_queue_has_retained(struct vnet_admin_queue_state *state);
void vnet_admin_queue_quiesce(struct vnet_admin_queue_state *state);
void vnet_admin_queue_reset(struct vnet_admin_queue_state *state);
void vnet_admin_queue_fini(struct vnet_admin_queue_state *state);
doca_error_t vnet_admin_queue_async_execute(struct vnet_admin_async_cmd *cmd);
void vnet_admin_queue_async_cancel(struct vnet_admin_async_cmd *cmd);
bool vnet_admin_queue_async_is_internal(const struct vnet_admin_async_cmd *cmd);
void vnet_admin_queue_dispatch_view(struct vnet_pci_dev_controller *ctrl,
				    const struct vnet_admin_cmd_view *view,
				    uint16_t opcode,
				    uint16_t group_type,
				    uint64_t group_member_id,
				    struct vnet_admin_cmd_result *result);
void vnet_admin_queue_dispatch_internal(struct vnet_pci_dev_controller *ctrl,
					const struct vnet_admin_cmd_view *view,
					uint16_t opcode,
					uint16_t group_type,
					uint64_t group_member_id,
					struct vnet_admin_cmd_result *result);
void vnet_admin_queue_dispatch(struct vnet_pci_dev_controller *ctrl,
			       struct doca_devemu_virtio_admin_req *req,
			       uint16_t opcode,
			       uint16_t group_type,
			       uint64_t group_member_id,
			       struct vnet_admin_cmd_result *result);

#endif /* VNET_ADMIN_QUEUE_H_ */
