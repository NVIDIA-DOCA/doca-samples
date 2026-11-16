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

#ifndef VNET_ADMIN_DEV_PARTS_H_
#define VNET_ADMIN_DEV_PARTS_H_

#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>

struct virtio_dev_part_hdr;
struct vnet_admin_cmd_result;
struct vnet_admin_cmd_view;
struct vnet_admin_dev_parts_restore_state;
struct vnet_admin_resource_obj;
struct vnet_vf_slot;

void vnet_admin_dev_parts_invalidate(struct vnet_vf_slot *slot);
void vnet_admin_dev_parts_snapshot_reset(struct vnet_admin_resource_obj *obj);
void vnet_admin_dev_parts_cvq_reset(struct vnet_vf_slot *slot);
void vnet_admin_dev_parts_metadata_get(struct vnet_admin_resource_obj *obj,
				       struct vnet_vf_slot *slot,
				       const struct vnet_admin_cmd_view *view,
				       uint8_t metadata_type,
				       struct vnet_admin_cmd_result *result);
void vnet_admin_dev_parts_get(struct vnet_admin_resource_obj *obj,
			      struct vnet_vf_slot *slot,
			      const struct vnet_admin_cmd_view *view,
			      struct vnet_admin_cmd_result *result);
void vnet_admin_dev_parts_get_selected(struct vnet_admin_resource_obj *obj,
				       struct vnet_vf_slot *slot,
				       const struct virtio_dev_part_hdr *hdrs,
				       uint32_t hdr_count,
				       const struct vnet_admin_cmd_view *view,
				       struct vnet_admin_cmd_result *result);
uint32_t vnet_admin_dev_parts_max_header_list_size(uint16_t vqs_count);
uint32_t vnet_admin_dev_parts_max_payload_size(uint16_t vqs_count);
doca_error_t vnet_admin_dev_parts_restore_state_create(struct vnet_vf_slot *slot,
						       const uint8_t *parts,
						       uint32_t parts_len,
						       struct vnet_admin_dev_parts_restore_state **restore_state);
void vnet_admin_dev_parts_restore_state_destroy(struct vnet_admin_dev_parts_restore_state *restore_state);
doca_error_t vnet_admin_dev_parts_restore_state_apply(struct vnet_admin_dev_parts_restore_state *restore_state,
						      struct vnet_vf_slot *slot,
						      uint64_t admin_generation,
						      bool *rebuild_required);

#endif /* VNET_ADMIN_DEV_PARTS_H_ */
