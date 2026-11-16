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

#ifndef VIRTIO_ADMIN_SPEC_H_
#define VIRTIO_ADMIN_SPEC_H_

#include <linux/types.h>
#include <linux/virtio_pci.h>

#ifdef VIRTIO_ADMIN_CMD_CAP_ID_LIST_QUERY
#define HAVE_KERNEL_VIRTIO_ADMIN_CAPS 1
#endif

#ifdef VIRTIO_ADMIN_CMD_RESOURCE_OBJ_CREATE
#define HAVE_KERNEL_VIRTIO_ADMIN_RESOURCE_OBJ 1
#endif

#ifdef VIRTIO_ADMIN_CMD_DEV_PARTS_GET
#define HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS 1
#endif

#ifdef VIRTIO_ADMIN_CMD_DEV_PARTS_SET
#define HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS_SET 1
#endif

#ifdef VIRTIO_ADMIN_CMD_DEV_MODE_SET
#define HAVE_KERNEL_VIRTIO_ADMIN_DEV_MODE_SET 1
#endif

#ifdef VIRTIO_DEV_PART_VQ_CFG
#define HAVE_KERNEL_VIRTIO_ADMIN_DEV_PART_VALUES 1
#endif

#undef VIRTIO_ADMIN_CMD_LIST_QUERY
#undef VIRTIO_ADMIN_CMD_LIST_USE
#undef VIRTIO_ADMIN_CMD_CAP_ID_LIST_QUERY
#undef VIRTIO_ADMIN_CMD_DEVICE_CAP_GET
#undef VIRTIO_ADMIN_CMD_DRIVER_CAP_SET
#undef VIRTIO_ADMIN_CMD_RESOURCE_OBJ_CREATE
#undef VIRTIO_ADMIN_CMD_RESOURCE_OBJ_DESTROY
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_GET
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_GET
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_SET
#undef VIRTIO_ADMIN_CMD_DEV_MODE_SET
#undef VIRTIO_ADMIN_GROUP_TYPE_SELF
#undef VIRTIO_ADMIN_GROUP_TYPE_SRIOV
#undef VIRTIO_ADMIN_STATUS_OK
#undef VIRTIO_ADMIN_STATUS_ENXIO
#undef VIRTIO_ADMIN_STATUS_EAGAIN
#undef VIRTIO_ADMIN_STATUS_ENOMEM
#undef VIRTIO_ADMIN_STATUS_EBUSY
#undef VIRTIO_ADMIN_STATUS_EINVAL
#undef VIRTIO_ADMIN_STATUS_ENOSPC
#undef VIRTIO_ADMIN_STATUS_Q_OK
#undef VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND
#undef VIRTIO_ADMIN_STATUS_Q_INVALID_OPCODE
#undef VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD
#undef VIRTIO_ADMIN_STATUS_Q_INVALID_GROUP
#undef VIRTIO_ADMIN_STATUS_Q_INVALID_MEMBER
#undef VIRTIO_ADMIN_STATUS_Q_NORESOURCE
#undef VIRTIO_ADMIN_STATUS_Q_TRYAGAIN
#undef VIRTIO_DEV_PARTS_CAP
#undef VIRTIO_RESOURCE_OBJ_DEV_PARTS
#undef VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_GET
#undef VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_SET
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_SIZE
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_COUNT
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_LIST
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_SELECTED
#undef VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_ALL
#undef VIRTIO_ADMIN_CMD_DEV_MODE_F_STOPPED
#undef VIRTIO_DEV_PART_F_OPTIONAL
#undef VIRTIO_DEV_PART_DEV_FEATURES
#undef VIRTIO_DEV_PART_DRV_FEATURES
#undef VIRTIO_DEV_PART_PCI_COMMON_CFG
#undef VIRTIO_DEV_PART_DEVICE_STATUS
#undef VIRTIO_DEV_PART_VQ_CFG
#undef VIRTIO_DEV_PART_VQ_NOTIFY_CFG

/* VirtIO Admin command opcodes */
#define VIRTIO_ADMIN_CMD_LIST_QUERY 0
#define VIRTIO_ADMIN_CMD_LIST_USE 1
#define VIRTIO_ADMIN_CMD_CAP_ID_LIST_QUERY 0x7
#define VIRTIO_ADMIN_CMD_DEVICE_CAP_GET 0x8
#define VIRTIO_ADMIN_CMD_DRIVER_CAP_SET 0x9
#define VIRTIO_ADMIN_CMD_RESOURCE_OBJ_CREATE 0xa
#define VIRTIO_ADMIN_CMD_RESOURCE_OBJ_DESTROY 0xd
#define VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_GET 0xe
#define VIRTIO_ADMIN_CMD_DEV_PARTS_GET 0xf
#define VIRTIO_ADMIN_CMD_DEV_PARTS_SET 0x10
#define VIRTIO_ADMIN_CMD_DEV_MODE_SET 0x11

/* VirtIO Admin command group types */
#define VIRTIO_ADMIN_GROUP_TYPE_SELF 0
#define VIRTIO_ADMIN_GROUP_TYPE_SRIOV 1

/* VirtIO Admin command status */
#define VIRTIO_ADMIN_STATUS_OK 0
#define VIRTIO_ADMIN_STATUS_ENXIO 6
#define VIRTIO_ADMIN_STATUS_EAGAIN 11
#define VIRTIO_ADMIN_STATUS_ENOMEM 12
#define VIRTIO_ADMIN_STATUS_EBUSY 16
#define VIRTIO_ADMIN_STATUS_EINVAL 22
#define VIRTIO_ADMIN_STATUS_ENOSPC 28
#define VIRTIO_ADMIN_STATUS_Q_OK 0
#define VIRTIO_ADMIN_STATUS_Q_INVALID_COMMAND 1
#define VIRTIO_ADMIN_STATUS_Q_INVALID_OPCODE 2
#define VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD 3
#define VIRTIO_ADMIN_STATUS_Q_INVALID_GROUP 4
#define VIRTIO_ADMIN_STATUS_Q_INVALID_MEMBER 5
#define VIRTIO_ADMIN_STATUS_Q_NORESOURCE 6
#define VIRTIO_ADMIN_STATUS_Q_TRYAGAIN 7

/* VirtIO Admin capabilities */
#define VIRTIO_DEV_PARTS_CAP 0

/* The application protocol currently advertises only capability ID 0. */
#define DOCA_ADMIN_CAP_ID_BITMAP_WORDS 1

#ifndef HAVE_KERNEL_VIRTIO_ADMIN_CAPS
struct virtio_admin_cmd_query_cap_id_result {
	__le64 supported_caps[DOCA_ADMIN_CAP_ID_BITMAP_WORDS];
} __attribute__((packed));

struct virtio_admin_cmd_cap_get_data {
	__le16 id;
	__u8 reserved[6];
} __attribute__((packed));

struct virtio_admin_cmd_cap_set_data {
	__le16 id;
	__u8 reserved[6];
	__u8 cap_specific_data[];
} __attribute__((packed));
#endif /* !HAVE_KERNEL_VIRTIO_ADMIN_CAPS */

#ifndef HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS
struct virtio_dev_parts_cap {
	__u8 get_parts_resource_objects_limit;
	__u8 set_parts_resource_objects_limit;
} __attribute__((packed));
#endif /* !HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS */

_Static_assert(sizeof(struct virtio_admin_cmd_query_cap_id_result) == 8, "admin capability ID list size");
_Static_assert(sizeof(struct virtio_admin_cmd_cap_get_data) == 8, "admin capability get data size");
_Static_assert(sizeof(struct virtio_dev_parts_cap) == 2, "admin device-parts capability size");
_Static_assert(sizeof(struct virtio_admin_cmd_cap_set_data) == 8, "admin capability set header size");

/* VirtIO Admin resource-object and device-parts definitions */
#define VIRTIO_RESOURCE_OBJ_DEV_PARTS 0
#define VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_GET 0
#define VIRTIO_RESOURCE_OBJ_DEV_PARTS_TYPE_SET 1

#define VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_SIZE 0
#define VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_COUNT 1
#define VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_LIST 2

#define VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_SELECTED 0
#define VIRTIO_ADMIN_CMD_DEV_PARTS_GET_TYPE_ALL 1
#define VIRTIO_ADMIN_CMD_DEV_MODE_F_STOPPED 0

#define VIRTIO_DEV_PART_F_OPTIONAL 0
#define VIRTIO_DEV_PART_DEV_FEATURES 0x100
#define VIRTIO_DEV_PART_DRV_FEATURES 0x101
#define VIRTIO_DEV_PART_PCI_COMMON_CFG 0x102
#define VIRTIO_DEV_PART_DEVICE_STATUS 0x103
#define VIRTIO_DEV_PART_VQ_CFG 0x104
#define VIRTIO_DEV_PART_VQ_NOTIFY_CFG 0x105

#ifndef HAVE_KERNEL_VIRTIO_ADMIN_RESOURCE_OBJ
struct virtio_admin_cmd_resource_obj_cmd_hdr {
	__le16 type;
	__u8 reserved[2];
	__le32 id;
} __attribute__((packed));

struct virtio_admin_cmd_resource_obj_create_data {
	struct virtio_admin_cmd_resource_obj_cmd_hdr hdr;
	__le64 flags;
	__u8 resource_obj_specific_data[];
} __attribute__((packed));
#endif /* !HAVE_KERNEL_VIRTIO_ADMIN_RESOURCE_OBJ */

#ifndef HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS
struct virtio_resource_obj_dev_parts {
	__u8 type;
	__u8 reserved[7];
} __attribute__((packed));

struct virtio_admin_cmd_dev_parts_metadata_data {
	struct virtio_admin_cmd_resource_obj_cmd_hdr hdr;
	__u8 type;
	__u8 reserved[7];
} __attribute__((packed));

struct virtio_dev_part_hdr {
	__le16 part_type;
	__u8 flags;
	__u8 reserved;
	union {
		struct {
			__le32 offset;
			__le32 reserved;
		} pci_common_cfg;
		struct {
			__le16 index;
			__u8 reserved[6];
		} vq_index;
		__u8 device_type_raw[8];
	} selector;
	__le32 length;
} __attribute__((packed));

struct virtio_admin_cmd_dev_parts_metadata_result {
	union {
		struct {
			__le32 size;
			__le32 reserved;
		} parts_size;
		struct {
			__le32 count;
			__le32 reserved;
		} hdr_list_count;
		struct {
			__le32 count;
			__le32 reserved;
			struct virtio_dev_part_hdr hdrs[];
		} hdr_list;
	};
} __attribute__((packed));

struct virtio_admin_cmd_dev_parts_get_data {
	struct virtio_admin_cmd_resource_obj_cmd_hdr hdr;
	__u8 type;
	__u8 reserved[7];
	struct virtio_dev_part_hdr hdr_list[];
} __attribute__((packed));

struct virtio_dev_part {
	struct virtio_dev_part_hdr hdr;
	__u8 value[];
} __attribute__((packed));
#endif /* !HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS */

#ifndef HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS_SET
struct virtio_admin_cmd_dev_parts_set_data {
	struct virtio_admin_cmd_resource_obj_cmd_hdr hdr;
	struct virtio_dev_part parts[];
} __attribute__((packed));
#endif /* !HAVE_KERNEL_VIRTIO_ADMIN_DEV_PARTS_SET */

#ifndef HAVE_KERNEL_VIRTIO_ADMIN_DEV_MODE_SET
struct virtio_admin_cmd_dev_mode_set_data {
	__u8 flags;
} __attribute__((packed));
#endif /* !HAVE_KERNEL_VIRTIO_ADMIN_DEV_MODE_SET */

#ifndef HAVE_KERNEL_VIRTIO_ADMIN_DEV_PART_VALUES
#define VNET_PCI_DEV_DECLARE_FLEX_ARRAY(TYPE, NAME) \
	struct { \
		struct { \
		} vnet_pci_dev_empty_##NAME; \
		TYPE NAME[]; \
	}

struct virtio_dev_part_features {
	VNET_PCI_DEV_DECLARE_FLEX_ARRAY(__le64, feature_bits);
} __attribute__((packed));

#undef VNET_PCI_DEV_DECLARE_FLEX_ARRAY

struct virtio_dev_part_vq_cfg {
	__le16 queue_size;
	__le16 vector;
	__le16 enabled;
	__le16 reserved;
	__le64 queue_desc;
	__le64 queue_driver;
	__le64 queue_device;
} __attribute__((packed));

struct virtio_dev_part_vq_notify_data {
	__le16 queue_notify_off;
	__le16 queue_notif_config_data;
	__u8 reserved[4];
} __attribute__((packed));
#endif /* !HAVE_KERNEL_VIRTIO_ADMIN_DEV_PART_VALUES */

_Static_assert(sizeof(struct virtio_admin_cmd_resource_obj_cmd_hdr) == 8, "admin resource object header size");
_Static_assert(sizeof(struct virtio_admin_cmd_resource_obj_create_data) == 16,
	       "admin resource object create header size");
_Static_assert(sizeof(struct virtio_resource_obj_dev_parts) == 8, "admin device-parts resource data size");
_Static_assert(sizeof(struct virtio_admin_cmd_dev_parts_metadata_data) == 16, "admin device-parts metadata data size");
_Static_assert(sizeof(struct virtio_admin_cmd_dev_parts_metadata_result) == 8,
	       "admin device-parts metadata result size");
_Static_assert(sizeof(struct virtio_admin_cmd_dev_parts_get_data) == 16, "admin device-parts get data size");
_Static_assert(sizeof(struct virtio_admin_cmd_dev_parts_set_data) == 8, "admin device-parts set header size");
_Static_assert(sizeof(struct virtio_admin_cmd_dev_mode_set_data) == 1, "admin device mode set data size");
_Static_assert(sizeof(struct virtio_dev_part_hdr) == 16, "admin device part header size");
_Static_assert(sizeof(struct virtio_dev_part_vq_cfg) == 32, "admin VQ config part size");
_Static_assert(sizeof(struct virtio_dev_part_vq_notify_data) == 8, "admin VQ notify part size");

#endif /* VIRTIO_ADMIN_SPEC_H_ */
