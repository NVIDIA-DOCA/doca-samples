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
#include <stdlib.h>
#include <string.h>

#include <doca_log.h>

#include "virtio_admin_spec.h"
#include "vnet_admin_dev_parts.h"
#include "vnet_admin_queue.h"
#include "vnet_admin_vf.h"
#include "vnet_pci_dev_core.h"
#include "vnet_pci_device.h"

DOCA_LOG_REGISTER(VNET_ADMIN_DEV_PARTS);

#define VNET_ADMIN_DEV_PARTS_FIXED_COUNT 9U
#define VNET_ADMIN_DEV_PARTS_PER_VQ_COUNT 2U
#define VNET_ADMIN_CAPTURE_RETRY_LIMIT 3U

enum vnet_admin_part_selector {
	VNET_ADMIN_PART_SELECTOR_NONE,
	VNET_ADMIN_PART_SELECTOR_PCI,
	VNET_ADMIN_PART_SELECTOR_VQ,
	VNET_ADMIN_PART_SELECTOR_CVQ,
};

enum vnet_admin_capture_status {
	VNET_ADMIN_CAPTURE_OK,
	VNET_ADMIN_CAPTURE_NORESOURCE,
	VNET_ADMIN_CAPTURE_TRYAGAIN,
};

struct vnet_admin_part_profile {
	uint32_t value_len;
	uint16_t type;
	uint16_t index;
	uint8_t flags;
	uint8_t selector;
	uint8_t class;
	uint8_t command;
};

struct vnet_admin_dev_parts_profile {
	/* The profile stores layout only; GET captures values for the same generation. */
	uint64_t generation;
	uint32_t count;
	uint32_t size;
	struct vnet_admin_part_profile parts[];
};

static void vnet_admin_dev_parts_result_set(struct vnet_admin_cmd_result *result, uint16_t qualifier)
{
	result->status_qualifier = qualifier;
	result->status = vnet_admin_status_from_qualifier(qualifier);
	result->len = 0;
}

static uint16_t vnet_admin_dev_parts_vq_indices_get(const struct vnet_virtio_queue_config *vqs,
						    uint16_t vqs_count,
						    uint16_t *vq_indices)
{
	uint16_t count = 0, i;

	for (i = 0; i < vqs_count; i++) {
		if (vqs[i].queue_desc != 0)
			vq_indices[count++] = i;
	}
	if (count == 0 && vqs_count != 0)
		vq_indices[count++] = 0;

	return count;
}

static uint32_t vnet_admin_dev_parts_profile_count(uint16_t num_vqs)
{
	return VNET_ADMIN_DEV_PARTS_FIXED_COUNT + VNET_ADMIN_DEV_PARTS_PER_VQ_COUNT * num_vqs;
}

uint32_t vnet_admin_dev_parts_max_header_list_size(uint16_t vqs_count)
{
	return vnet_admin_dev_parts_profile_count(vqs_count) * sizeof(struct virtio_dev_part_hdr);
}

uint32_t vnet_admin_dev_parts_max_payload_size(uint16_t vqs_count)
{
	uint32_t profile_count = vnet_admin_dev_parts_profile_count(vqs_count);

	return profile_count * sizeof(struct virtio_dev_part_hdr) +
	       (sizeof(struct virtio_dev_part_features) + sizeof(uint64_t)) * 2 +
	       sizeof(struct vnet_virtio_dev_part_pci_common_cfg) + sizeof(uint8_t) +
	       sizeof(struct vnet_virtio_dev_part_net_config) +
	       vqs_count * (sizeof(struct virtio_dev_part_vq_cfg) + sizeof(struct virtio_dev_part_vq_notify_data)) +
	       sizeof(uint16_t) + ETH_ALEN + sizeof(struct vnet_virtio_net_ctrl_mac_table) +
	       VNET_VF_CVQ_MAX_MAC_ENTRIES * ETH_ALEN + sizeof(struct vnet_virtio_dev_part_vlan_table) +
	       VNET_VF_CVQ_MAX_VLAN_ENTRIES * sizeof(uint16_t);
}

static struct vnet_admin_dev_parts_profile *vnet_admin_dev_parts_profile_create(const uint16_t *vq_indices,
										uint16_t num_vqs,
										uint32_t mac_entries,
										uint16_t vlan_entries,
										uint64_t generation)
{
	struct vnet_admin_dev_parts_profile *profile;
	uint32_t profile_count, pos = 0, i;

	profile_count = vnet_admin_dev_parts_profile_count(num_vqs);
	profile = calloc(1, sizeof(*profile) + profile_count * sizeof(profile->parts[0]));
	if (profile == NULL)
		return NULL;
	profile->generation = generation;
	profile->count = profile_count;

#define VNET_ADMIN_PROFILE_ADD(_type, _flags, _selector, _len) \
	do { \
		profile->parts[pos].type = (_type); \
		profile->parts[pos].flags = (_flags); \
		profile->parts[pos].selector = (_selector); \
		profile->parts[pos].value_len = (_len); \
		pos++; \
	} while (0)

	VNET_ADMIN_PROFILE_ADD(VIRTIO_DEV_PART_DEV_FEATURES,
			       1U << VIRTIO_DEV_PART_F_OPTIONAL,
			       VNET_ADMIN_PART_SELECTOR_NONE,
			       sizeof(struct virtio_dev_part_features) + sizeof(uint64_t));
	VNET_ADMIN_PROFILE_ADD(VIRTIO_DEV_PART_DRV_FEATURES,
			       0,
			       VNET_ADMIN_PART_SELECTOR_NONE,
			       sizeof(struct virtio_dev_part_features) + sizeof(uint64_t));
	VNET_ADMIN_PROFILE_ADD(VIRTIO_DEV_PART_PCI_COMMON_CFG,
			       0,
			       VNET_ADMIN_PART_SELECTOR_PCI,
			       sizeof(struct vnet_virtio_dev_part_pci_common_cfg));
	VNET_ADMIN_PROFILE_ADD(VIRTIO_DEV_PART_DEVICE_STATUS, 0, VNET_ADMIN_PART_SELECTOR_NONE, sizeof(uint8_t));
	VNET_ADMIN_PROFILE_ADD(VNET_ADMIN_DEV_PART_NET_CFG,
			       0,
			       VNET_ADMIN_PART_SELECTOR_NONE,
			       sizeof(struct vnet_virtio_dev_part_net_config));

	for (i = 0; i < num_vqs; i++) {
		VNET_ADMIN_PROFILE_ADD(VIRTIO_DEV_PART_VQ_CFG,
				       0,
				       VNET_ADMIN_PART_SELECTOR_VQ,
				       sizeof(struct virtio_dev_part_vq_cfg));
		profile->parts[pos - 1].index = vq_indices[i];
	}

	for (i = 0; i < num_vqs; i++) {
		VNET_ADMIN_PROFILE_ADD(VIRTIO_DEV_PART_VQ_NOTIFY_CFG,
				       0,
				       VNET_ADMIN_PART_SELECTOR_VQ,
				       sizeof(struct virtio_dev_part_vq_notify_data));
		profile->parts[pos - 1].index = vq_indices[i];
	}

	VNET_ADMIN_PROFILE_ADD(VIRTIO_NET_DEV_PART_CVQ_CFG_PART, 0, VNET_ADMIN_PART_SELECTOR_CVQ, sizeof(uint16_t));
	profile->parts[pos - 1].class = VIRTIO_NET_CTRL_MQ;
	profile->parts[pos - 1].command = VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET;
	VNET_ADMIN_PROFILE_ADD(VIRTIO_NET_DEV_PART_CVQ_CFG_PART, 0, VNET_ADMIN_PART_SELECTOR_CVQ, ETH_ALEN);
	profile->parts[pos - 1].class = VIRTIO_NET_CTRL_MAC;
	profile->parts[pos - 1].command = VIRTIO_NET_CTRL_MAC_ADDR_SET;
	VNET_ADMIN_PROFILE_ADD(VIRTIO_NET_DEV_PART_CVQ_CFG_PART,
			       0,
			       VNET_ADMIN_PART_SELECTOR_CVQ,
			       sizeof(struct vnet_virtio_net_ctrl_mac_table) + mac_entries * ETH_ALEN);
	profile->parts[pos - 1].class = VIRTIO_NET_CTRL_MAC;
	profile->parts[pos - 1].command = VIRTIO_NET_CTRL_MAC_TABLE_SET;
	VNET_ADMIN_PROFILE_ADD(VIRTIO_NET_DEV_PART_CVQ_CFG_PART,
			       0,
			       VNET_ADMIN_PART_SELECTOR_CVQ,
			       sizeof(struct vnet_virtio_dev_part_vlan_table) + vlan_entries * sizeof(uint16_t));
	profile->parts[pos - 1].class = VIRTIO_NET_CTRL_VLAN;
	profile->parts[pos - 1].command = VIRTIO_NET_CTRL_VLAN_ADD;

#undef VNET_ADMIN_PROFILE_ADD

	for (i = 0; i < profile_count; i++)
		profile->size += sizeof(struct virtio_dev_part_hdr) + profile->parts[i].value_len;
	return profile;
}

static void vnet_admin_dev_parts_hdr_encode(const struct vnet_admin_part_profile *profile,
					    struct virtio_dev_part_hdr *hdr)
{
	memset(hdr, 0, sizeof(*hdr));

	hdr->part_type = htole16(profile->type);
	hdr->flags = profile->flags;
	hdr->length = htole32(profile->value_len);
	switch (profile->selector) {
	case VNET_ADMIN_PART_SELECTOR_PCI:
		/* The current app exports the complete common config at offset zero. */
		hdr->selector.pci_common_cfg.offset = htole32(0);
		break;
	case VNET_ADMIN_PART_SELECTOR_VQ:
		hdr->selector.vq_index.index = htole16(profile->index);
		break;
	case VNET_ADMIN_PART_SELECTOR_CVQ: {
		/* Some kernel headers omit device_type_raw but retain the eight-byte selector union. */
		struct vnet_virtio_dev_part_cvq_selector *cvq = (void *)&hdr->selector;

		cvq->class = profile->class;
		cvq->command = profile->command;
		break;
	}
	default:
		break;
	}
}

static void vnet_admin_dev_parts_common_encode(const struct vnet_pci_device *vf,
					       struct vnet_virtio_dev_part_pci_common_cfg *wire)
{
	const struct vnet_virtio_common_config *cfg = &vf->pci_cfg;

	memset(wire, 0, sizeof(*wire));

	wire->device_feature_select = htole32(cfg->device_feature_select);
	wire->device_feature = htole32(cfg->device_feature);
	wire->driver_feature_select = htole32(cfg->driver_feature_select);
	wire->driver_feature = htole32(cfg->driver_feature);
	wire->config_msix_vector = htole16(cfg->config_msix_vector);
	wire->num_queues = htole16(cfg->num_queues);
	wire->config_generation = cfg->config_generation;
	wire->queue_select = htole16(cfg->queue_select);
}

static void vnet_admin_dev_parts_net_encode(const struct vnet_virtio_net_config *net,
					    struct vnet_virtio_dev_part_net_config *wire)
{
	memcpy(wire->mac, net->mac, sizeof(wire->mac));
	wire->status = htole16(net->status);
	wire->max_virtqueue_pairs = htole16(net->max_virtqueue_pairs);
	wire->mtu = htole16(net->mtu);
}

static void vnet_admin_dev_parts_vq_encode(const struct vnet_virtio_queue_config *vq,
					   struct virtio_dev_part_vq_cfg *wire)
{
	wire->queue_size = htole16(vq->queue_size);
	wire->vector = htole16(vq->queue_msix_vector);
	wire->enabled = htole16(vq->queue_enable);
	wire->queue_desc = htole64(vq->queue_desc);
	wire->queue_driver = htole64(vq->queue_driver);
	wire->queue_device = htole64(vq->queue_device);
}

static void vnet_admin_dev_parts_vq_notify_encode(const struct vnet_virtio_queue_config *vq,
						  struct virtio_dev_part_vq_notify_data *wire)
{
	wire->queue_notify_off = htole16(vq->queue_notify_off);
	wire->queue_notif_config_data = htole16(vq->queue_notif_config_data);
}

static void vnet_admin_dev_parts_cvq_encode(const struct vnet_admin_part_profile *part,
					    const struct vnet_vf_slot *slot,
					    const struct vnet_virtio_net_config *net,
					    const struct vnet_vf_cvq_state *cvq_state,
					    uint8_t *value)
{
	struct vnet_virtio_dev_part_vlan_table *vlan_table;
	struct vnet_virtio_net_ctrl_mac_table *mac_table;
	uint16_t active_qps, entry = 0, vlan;

	if (part->class == VIRTIO_NET_CTRL_MQ) {
		active_qps = (slot->vf_dev->pci_cfg.device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK) != 0 ?
				     atomic_load(&slot->num_active_qps) :
				     0;
		memcpy(value, &(uint16_t){htole16(active_qps)}, sizeof(active_qps));
		return;
	}
	if (part->class == VIRTIO_NET_CTRL_MAC && part->command == VIRTIO_NET_CTRL_MAC_ADDR_SET) {
		memcpy(value, net->mac, ETH_ALEN);
		return;
	}
	if (part->class == VIRTIO_NET_CTRL_MAC && part->command == VIRTIO_NET_CTRL_MAC_TABLE_SET) {
		mac_table = (struct vnet_virtio_net_ctrl_mac_table *)value;
		mac_table->entries = htole32(cvq_state->mac_entries);
		memcpy(mac_table->macs, cvq_state->macs, cvq_state->mac_entries * ETH_ALEN);
		return;
	}
	if (part->class != VIRTIO_NET_CTRL_VLAN || part->command != VIRTIO_NET_CTRL_VLAN_ADD)
		return;

	vlan_table = (struct vnet_virtio_dev_part_vlan_table *)value;
	vlan_table->num_entries = htole16(cvq_state->vlan_entries);
	for (vlan = 0; vlan < VNET_VF_CVQ_MAX_VLAN_ENTRIES; vlan++) {
		if ((cvq_state->vlan_bitmap[vlan / 64] & (1ULL << (vlan % 64))) != 0)
			vlan_table->vlan[entry++] = htole16(vlan);
	}
}

static void vnet_admin_dev_parts_value_encode(const struct vnet_admin_part_profile *part,
					      const struct vnet_vf_slot *slot,
					      const struct vnet_virtio_net_config *net,
					      const struct vnet_virtio_queue_config *vqs,
					      const struct vnet_vf_cvq_state *cvq_state,
					      uint8_t *value)
{
	const struct vnet_virtio_queue_config *vq;

	switch (part->type) {
	case VIRTIO_DEV_PART_DEV_FEATURES:
		((struct virtio_dev_part_features *)value)->feature_bits[0] = htole64(slot->vf_dev->device_features);
		break;
	case VIRTIO_DEV_PART_DRV_FEATURES:
		((struct virtio_dev_part_features *)value)->feature_bits[0] = htole64(slot->vf_dev->driver_features);
		break;
	case VIRTIO_DEV_PART_PCI_COMMON_CFG:
		vnet_admin_dev_parts_common_encode(slot->vf_dev, (struct vnet_virtio_dev_part_pci_common_cfg *)value);
		break;
	case VIRTIO_DEV_PART_DEVICE_STATUS:
		*value = slot->vf_dev->pci_cfg.device_status;
		break;
	case VNET_ADMIN_DEV_PART_NET_CFG:
		vnet_admin_dev_parts_net_encode(net, (struct vnet_virtio_dev_part_net_config *)value);
		break;
	case VIRTIO_DEV_PART_VQ_CFG:
		vq = &vqs[part->index];
		vnet_admin_dev_parts_vq_encode(vq, (struct virtio_dev_part_vq_cfg *)value);
		break;
	case VIRTIO_DEV_PART_VQ_NOTIFY_CFG:
		vq = &vqs[part->index];
		vnet_admin_dev_parts_vq_notify_encode(vq, (struct virtio_dev_part_vq_notify_data *)value);
		break;
	case VIRTIO_NET_DEV_PART_CVQ_CFG_PART:
		vnet_admin_dev_parts_cvq_encode(part, slot, net, cvq_state, value);
		break;
	default:
		break;
	}
}

static bool vnet_admin_dev_parts_state_capture(struct vnet_vf_slot *slot,
					       struct vnet_virtio_net_config *net,
					       struct vnet_virtio_queue_config **vqs,
					       struct vnet_vf_cvq_state *cvq_state,
					       uint64_t *generation)
{
	*generation = atomic_load(&slot->admin_cache_generation);
	*vqs = calloc(slot->vf_dev->vqs_count, sizeof(**vqs));
	if (*vqs == NULL)
		return false;

	pthread_mutex_lock(&slot->vf_dev->vqs_mutex);
	if (net != NULL)
		memcpy(net, &slot->vf_dev->vnet_cfg, sizeof(*net));
	memcpy(*vqs, slot->vf_dev->vqs, slot->vf_dev->vqs_count * sizeof(**vqs));
	memcpy(cvq_state, &slot->cvq_state, sizeof(*cvq_state));
	pthread_mutex_unlock(&slot->vf_dev->vqs_mutex);
	return true;
}

void vnet_admin_dev_parts_snapshot_reset(struct vnet_admin_resource_obj *obj)
{
	free(obj->profile);
	free(obj->cache);
	obj->profile = NULL;
	obj->cache = NULL;
	obj->cache_generation = 0;
	obj->cache_len = 0;
}

static enum vnet_admin_capture_status vnet_admin_dev_parts_profile_ensure(struct vnet_admin_resource_obj *obj,
									  struct vnet_vf_slot *slot)
{
	struct vnet_admin_dev_parts_profile *profile;
	struct vnet_vf_cvq_state cvq_state = {0};
	struct vnet_virtio_queue_config *vqs;
	uint16_t *vq_indices;
	uint64_t generation;
	uint32_t attempt;
	uint16_t num_vqs;

	for (attempt = 0; attempt < VNET_ADMIN_CAPTURE_RETRY_LIMIT; attempt++) {
		generation = atomic_load(&slot->admin_cache_generation);
		if (obj->profile != NULL && obj->profile->generation == generation)
			return VNET_ADMIN_CAPTURE_OK;
		if (!vnet_admin_dev_parts_state_capture(slot, NULL, &vqs, &cvq_state, &generation))
			return VNET_ADMIN_CAPTURE_NORESOURCE;
		vq_indices = calloc(slot->vf_dev->vqs_count, sizeof(*vq_indices));
		if (vq_indices == NULL) {
			free(vqs);
			return VNET_ADMIN_CAPTURE_NORESOURCE;
		}
		num_vqs = vnet_admin_dev_parts_vq_indices_get(vqs, slot->vf_dev->vqs_count, vq_indices);
		profile = vnet_admin_dev_parts_profile_create(vq_indices,
							      num_vqs,
							      cvq_state.mac_entries,
							      cvq_state.vlan_entries,
							      generation);
		free(vq_indices);
		free(vqs);
		if (profile == NULL)
			return VNET_ADMIN_CAPTURE_NORESOURCE;

		/* Retry when the generated profile is stale before publication. */
		if (generation != atomic_load(&slot->admin_cache_generation)) {
			free(profile);
			continue;
		}

		vnet_admin_dev_parts_snapshot_reset(obj);
		obj->profile = profile;
		return VNET_ADMIN_CAPTURE_OK;
	}
	return VNET_ADMIN_CAPTURE_TRYAGAIN;
}

static enum vnet_admin_capture_status vnet_admin_dev_parts_cache_create(struct vnet_admin_resource_obj *obj,
									struct vnet_vf_slot *slot)
{
	struct vnet_admin_dev_parts_profile *profile;
	struct vnet_virtio_net_config net;
	struct vnet_vf_cvq_state cvq_state = {0};
	struct vnet_virtio_queue_config *vqs;
	struct virtio_dev_part_hdr *hdr;
	uint8_t *cursor, *cache;
	enum vnet_admin_capture_status status;
	uint64_t generation;
	uint32_t attempt, i;

	for (attempt = 0; attempt < VNET_ADMIN_CAPTURE_RETRY_LIMIT; attempt++) {
		status = vnet_admin_dev_parts_profile_ensure(obj, slot);
		if (status != VNET_ADMIN_CAPTURE_OK)
			return status;
		profile = obj->profile;

		if (!vnet_admin_dev_parts_state_capture(slot, &net, &vqs, &cvq_state, &generation))
			return VNET_ADMIN_CAPTURE_NORESOURCE;

		/* Retry when capture and profile generations do not match. */
		if (generation != profile->generation) {
			free(vqs);
			continue;
		}

		cache = calloc(1, profile->size);
		if (cache == NULL) {
			free(vqs);
			return VNET_ADMIN_CAPTURE_NORESOURCE;
		}

		/* GET encodes both headers and snapshot values as repeated [header][value] records. */
		cursor = cache;
		for (i = 0; i < profile->count; i++) {
			hdr = (struct virtio_dev_part_hdr *)cursor;
			vnet_admin_dev_parts_hdr_encode(&profile->parts[i], hdr);
			cursor += sizeof(*hdr);
			vnet_admin_dev_parts_value_encode(&profile->parts[i], slot, &net, vqs, &cvq_state, cursor);
			cursor += profile->parts[i].value_len;
		}
		free(vqs);

		/* Retry when the encoded cache is stale before publication. */
		if (generation != atomic_load(&slot->admin_cache_generation)) {
			free(cache);
			continue;
		}

		free(obj->cache);
		obj->cache = cache;
		obj->cache_len = profile->size;
		obj->cache_generation = generation;
		return VNET_ADMIN_CAPTURE_OK;
	}
	return VNET_ADMIN_CAPTURE_TRYAGAIN;
}

static uint8_t *vnet_admin_dev_parts_metadata_result_create(const struct vnet_admin_dev_parts_profile *profile,
							    uint8_t metadata_type,
							    uint32_t *wire_len)
{
	struct virtio_admin_cmd_dev_parts_metadata_result *wire;
	uint32_t i;

	if (metadata_type == VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_LIST)
		*wire_len = sizeof(*wire) + profile->count * sizeof(struct virtio_dev_part_hdr);
	else
		*wire_len = sizeof(*wire);
	wire = calloc(1, *wire_len);
	if (wire == NULL)
		return NULL;

	switch (metadata_type) {
	case VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_SIZE:
		wire->parts_size.size = htole32(profile->size);
		break;
	case VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_COUNT:
		wire->hdr_list_count.count = htole32(profile->count);
		break;
	case VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_LIST:
		wire->hdr_list.count = htole32(profile->count);
		/* Metadata encodes headers only; values are captured lazily by GET. */
		for (i = 0; i < profile->count; i++)
			vnet_admin_dev_parts_hdr_encode(&profile->parts[i], &wire->hdr_list.hdrs[i]);
		break;
	default:
		free(wire);
		return NULL;
	}
	return (uint8_t *)wire;
}

struct vnet_admin_dev_parts_restore_state {
	struct vnet_virtio_dev_part_pci_common_cfg common;
	struct vnet_virtio_dev_part_net_config net;
	struct vnet_virtio_queue_config *vqs;
	struct vnet_vf_cvq_state cvq_state;
	uint64_t driver_features;
	uint16_t active_qps;
	uint8_t device_status;
	/* Primary MAC from MAC_ADDR_SET; cvq_state.macs holds filter-table entries. */
	uint8_t primary_mac[ETH_ALEN];
};

static bool vnet_admin_dev_parts_bytes_zero(const void *data, size_t len)
{
	const uint8_t *bytes = data;
	size_t i;

	for (i = 0; i < len; i++) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}

/* Checks whether a wire header represents the expected profile entry. */
static bool vnet_admin_dev_parts_header_matches(const struct virtio_dev_part_hdr *hdr,
						const struct vnet_admin_part_profile *profile)
{
	struct virtio_dev_part_hdr expected;

	vnet_admin_dev_parts_hdr_encode(profile, &expected);
	if (profile->type == VIRTIO_NET_DEV_PART_CVQ_CFG_PART &&
	    ((profile->class == VIRTIO_NET_CTRL_MAC && profile->command == VIRTIO_NET_CTRL_MAC_TABLE_SET) ||
	     (profile->class == VIRTIO_NET_CTRL_VLAN && profile->command == VIRTIO_NET_CTRL_VLAN_ADD)))
		expected.length = hdr->length;
	return memcmp(hdr, &expected, sizeof(expected)) == 0;
}

/* Collects the ordered unique queue indices needed to build a SET profile. */
static bool vnet_admin_dev_parts_vq_cardinality_get(const uint8_t *parts,
						    uint32_t parts_len,
						    uint16_t vqs_count,
						    uint16_t *vq_indices,
						    uint16_t *num_vqs)
{
	const struct virtio_dev_part_hdr *hdr;
	const uint8_t *cursor = parts;
	uint32_t remaining = parts_len, value_len;
	uint16_t last_config = 0, last_notify = 0;
	uint16_t count = 0, index, pos;
	bool have_config = false, have_notify = false;

	while (remaining != 0) {
		if (remaining < sizeof(*hdr))
			return false;
		hdr = (const struct virtio_dev_part_hdr *)cursor;
		value_len = le32toh(hdr->length);
		if (value_len > remaining - sizeof(*hdr))
			return false;

		switch (le16toh(hdr->part_type)) {
		case VIRTIO_DEV_PART_VQ_CFG:
			index = le16toh(hdr->selector.vq_index.index);
			if (have_notify || index >= vqs_count || (have_config && index <= last_config))
				return false;
			last_config = index;
			have_config = true;
			break;
		case VIRTIO_DEV_PART_VQ_NOTIFY_CFG:
			index = le16toh(hdr->selector.vq_index.index);
			if (index >= vqs_count || (have_notify && index <= last_notify))
				return false;
			last_notify = index;
			have_notify = true;
			break;
		default:
			goto next;
		}

		for (pos = 0; pos < count && vq_indices[pos] < index; pos++)
			;
		if (pos == count || vq_indices[pos] != index) {
			memmove(&vq_indices[pos + 1], &vq_indices[pos], (count - pos) * sizeof(*vq_indices));
			vq_indices[pos] = index;
			count++;
		}
next:
		cursor += sizeof(*hdr) + value_len;
		remaining -= sizeof(*hdr) + value_len;
	}

	*num_vqs = count;
	return true;
}

static bool vnet_admin_dev_parts_type_is_known(uint16_t type)
{
	switch (type) {
	case VIRTIO_DEV_PART_DEV_FEATURES:
	case VIRTIO_DEV_PART_DRV_FEATURES:
	case VIRTIO_DEV_PART_PCI_COMMON_CFG:
	case VIRTIO_DEV_PART_DEVICE_STATUS:
	case VNET_ADMIN_DEV_PART_NET_CFG:
	case VIRTIO_DEV_PART_VQ_CFG:
	case VIRTIO_DEV_PART_VQ_NOTIFY_CFG:
	case VIRTIO_NET_DEV_PART_CVQ_CFG_PART:
		return true;
	default:
		return false;
	}
}

/* Validates restored common configuration against the VF and decoded features. */
static bool vnet_admin_dev_parts_common_validate(const struct vnet_admin_dev_parts_restore_state *restore_state,
						 const struct vnet_vf_slot *slot,
						 const struct vnet_virtio_dev_part_pci_common_cfg *common)
{
	const struct vnet_pci_device *vf = slot->vf_dev;
	uint32_t feature_select = le32toh(common->device_feature_select);
	uint32_t driver_select = le32toh(common->driver_feature_select);
	uint32_t offered_feature, driver_feature;

	if (feature_select > 1 || driver_select > 1)
		return false;
	offered_feature = (uint32_t)(vf->device_features >> (feature_select * 32));
	driver_feature = (uint32_t)(restore_state->driver_features >> (driver_select * 32));
	if (le32toh(common->device_feature) != offered_feature ||
	    le16toh(common->num_queues) != vf->pci_cfg.num_queues ||
	    le32toh(common->driver_feature) != driver_feature || le16toh(common->queue_select) >= vf->vqs_count ||
	    common->device_status != 0 || !vnet_admin_dev_parts_bytes_zero(&common->vq, sizeof(common->vq)) ||
	    le16toh(common->admin_queue_index) != 0 || le16toh(common->admin_queue_num) != 0)
		return false;
	return true;
}

/* Validates queue geometry and the addresses required by an enabled queue. */
static bool vnet_admin_dev_parts_vq_validate(const struct virtio_dev_part_vq_cfg *vq)
{
	uint16_t queue_size = le16toh(vq->queue_size), enabled = le16toh(vq->enabled);

	if (vq->reserved != 0 || enabled > 1 || queue_size < VNET_MIN_QUEUE_SIZE || queue_size > VNET_MAX_QUEUE_SIZE ||
	    (queue_size & (queue_size - 1)) != 0)
		return false;
	if (enabled != 0 &&
	    (le64toh(vq->queue_desc) == 0 || le64toh(vq->queue_driver) == 0 || le64toh(vq->queue_device) == 0))
		return false;
	return true;
}

static bool vnet_admin_dev_parts_queue_is_enabled(const struct vnet_admin_dev_parts_restore_state *restore_state,
						  const struct vnet_vf_slot *slot,
						  uint16_t index)
{
	const struct vnet_virtio_queue_config *vq;

	if (index >= slot->vf_dev->vqs_count)
		return false;
	vq = &restore_state->vqs[index];
	return vq->queue_enable != 0 && vq->queue_size != 0 && vq->queue_desc != 0 && vq->queue_driver != 0 &&
	       vq->queue_device != 0;
}

/* Validates cross-part feature, status, and queue-state invariants. */
static bool vnet_admin_dev_parts_restore_state_validate(const struct vnet_admin_dev_parts_restore_state *restore_state,
							const struct vnet_vf_slot *slot)
{
	const struct vnet_pci_device *vf = slot->vf_dev;
	uint16_t max_qps, qp;
	bool features_ok = (restore_state->device_status & VNET_VIRTIO_DEVICE_STATUS_FEATURES_OK) != 0;
	bool driver_ok = (restore_state->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK) != 0;
	bool mq = (restore_state->driver_features & (1ULL << VIRTIO_NET_F_MQ)) != 0;
	bool tx_enabled, rx_enabled;

	max_qps = vf->vnet_cfg.max_virtqueue_pairs;
	if (!vnet_admin_vf_features_valid(restore_state->driver_features))
		return false;
	if (driver_ok && !features_ok)
		return false;
	if (features_ok &&
	    (restore_state->device_status & (VNET_VIRTIO_DEVICE_STATUS_ACK | VNET_VIRTIO_DEVICE_STATUS_DRIVER)) !=
		    (VNET_VIRTIO_DEVICE_STATUS_ACK | VNET_VIRTIO_DEVICE_STATUS_DRIVER))
		return false;
	if ((!driver_ok && restore_state->active_qps != 0) ||
	    (driver_ok && (restore_state->active_qps == 0 || restore_state->active_qps > max_qps)))
		return false;
	if (driver_ok && !mq && restore_state->active_qps != 1)
		return false;

	for (qp = 0; qp < max_qps; qp++) {
		tx_enabled = restore_state->vqs[qp * 2 + 1].queue_enable != 0;
		rx_enabled = restore_state->vqs[qp * 2].queue_enable != 0;

		if (rx_enabled != tx_enabled)
			return false;
		if (driver_ok && qp < restore_state->active_qps &&
		    (!vnet_admin_dev_parts_queue_is_enabled(restore_state, slot, (uint16_t)(qp * 2)) ||
		     !vnet_admin_dev_parts_queue_is_enabled(restore_state, slot, (uint16_t)(qp * 2 + 1))))
			return false;
	}
	if (driver_ok && mq && !vnet_admin_dev_parts_queue_is_enabled(restore_state, slot, VNET_CVQ_INDEX(max_qps)))
		return false;
	if (!mq && VNET_CVQ_INDEX(max_qps) < vf->vqs_count &&
	    restore_state->vqs[VNET_CVQ_INDEX(max_qps)].queue_enable != 0)
		return false;
	return true;
}

static bool vnet_admin_dev_parts_device_status_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						      const uint8_t *value)
{
	restore_state->device_status = *value;
	return (restore_state->device_status &
		~(VNET_VIRTIO_DEVICE_STATUS_ACK | VNET_VIRTIO_DEVICE_STATUS_DRIVER |
		  VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK | VNET_VIRTIO_DEVICE_STATUS_FEATURES_OK |
		  VNET_VIRTIO_DEVICE_STATUS_NEEDS_RESET | VNET_VIRTIO_DEVICE_STATUS_FAILED)) == 0;
}

static bool vnet_admin_dev_parts_net_config_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						   const struct vnet_vf_slot *slot,
						   const uint8_t *value)
{
	const struct vnet_virtio_dev_part_net_config *net = (const struct vnet_virtio_dev_part_net_config *)value;

	if (net->transitional != 0 || net->vq_pair_n != 0 || net->speed != 0 || net->duplex != 0 ||
	    net->rss_max_key_size != 0 || net->rss_max_indirection_table_length != 0 ||
	    net->supported_hash_types != 0 || net->supported_tunnel_types != 0 ||
	    le16toh(net->max_virtqueue_pairs) != slot->vf_dev->vnet_cfg.max_virtqueue_pairs || le16toh(net->mtu) == 0)
		return false;
	memcpy(&restore_state->net, net, sizeof(restore_state->net));
	return true;
}

static bool vnet_admin_dev_parts_vq_config_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						  const struct vnet_admin_part_profile *part,
						  const uint8_t *value)
{
	const struct virtio_dev_part_vq_cfg *vq = (const struct virtio_dev_part_vq_cfg *)value;
	struct vnet_virtio_queue_config *dst = &restore_state->vqs[part->index];

	if (!vnet_admin_dev_parts_vq_validate(vq))
		return false;
	dst->queue_size = le16toh(vq->queue_size);
	dst->queue_msix_vector = le16toh(vq->vector);
	dst->queue_enable = le16toh(vq->enabled);
	dst->queue_desc = le64toh(vq->queue_desc);
	dst->queue_driver = le64toh(vq->queue_driver);
	dst->queue_device = le64toh(vq->queue_device);
	return true;
}

static bool vnet_admin_dev_parts_vq_notify_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						  struct vnet_vf_slot *slot,
						  const struct vnet_admin_part_profile *part,
						  const uint8_t *value)
{
	const struct virtio_dev_part_vq_notify_data *notify = (const struct virtio_dev_part_vq_notify_data *)value;
	struct vnet_virtio_queue_config *dst = &restore_state->vqs[part->index];
	struct vnet_virtio_queue_config current = {0};

	if (!vnet_pci_device_snapshot_vq(slot->vf_dev, part->index, &current) ||
	    !vnet_admin_dev_parts_bytes_zero(notify->reserved, sizeof(notify->reserved)) ||
	    le16toh(notify->queue_notify_off) != current.queue_notify_off ||
	    le16toh(notify->queue_notif_config_data) != current.queue_notif_config_data)
		return false;
	dst->queue_notify_off = current.queue_notify_off;
	dst->queue_notif_config_data = current.queue_notif_config_data;
	return true;
}

static bool vnet_admin_dev_parts_cvq_mq_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
					       const struct vnet_vf_slot *slot,
					       const uint8_t *value)
{
	memcpy(&restore_state->active_qps, value, sizeof(restore_state->active_qps));
	restore_state->active_qps = le16toh(restore_state->active_qps);
	return restore_state->active_qps <= slot->vf_dev->vnet_cfg.max_virtqueue_pairs;
}

static bool vnet_admin_dev_parts_cvq_mac_table_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						      uint32_t value_len,
						      const uint8_t *value)
{
	const struct vnet_virtio_net_ctrl_mac_table *mac_table = (const struct vnet_virtio_net_ctrl_mac_table *)value;
	uint32_t entries;

	if (value_len < sizeof(*mac_table))
		return false;
	entries = le32toh(mac_table->entries);
	if (entries > VNET_VF_CVQ_MAX_MAC_ENTRIES || value_len != sizeof(*mac_table) + entries * ETH_ALEN)
		return false;
	memset(restore_state->cvq_state.macs, 0, sizeof(restore_state->cvq_state.macs));
	restore_state->cvq_state.mac_entries = entries;
	memcpy(restore_state->cvq_state.macs, mac_table->macs, entries * ETH_ALEN);
	return true;
}

static bool vnet_admin_dev_parts_cvq_mac_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						const struct vnet_admin_part_profile *part,
						uint32_t value_len,
						const uint8_t *value)
{
	switch (part->command) {
	case VIRTIO_NET_CTRL_MAC_ADDR_SET:
		memcpy(restore_state->primary_mac, value, sizeof(restore_state->primary_mac));
		return true;
	case VIRTIO_NET_CTRL_MAC_TABLE_SET:
		return vnet_admin_dev_parts_cvq_mac_table_decode(restore_state, value_len, value);
	default:
		return false;
	}
}

static bool vnet_admin_dev_parts_cvq_vlan_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						 uint32_t value_len,
						 const uint8_t *value)
{
	const struct vnet_virtio_dev_part_vlan_table *vlan = (const struct vnet_virtio_dev_part_vlan_table *)value;
	uint16_t entries, vid, i;

	if (value_len < sizeof(*vlan) || !vnet_admin_dev_parts_bytes_zero(vlan->reserved, sizeof(vlan->reserved)))
		return false;
	entries = le16toh(vlan->num_entries);
	if (entries > VNET_VF_CVQ_MAX_VLAN_ENTRIES || value_len != sizeof(*vlan) + entries * sizeof(uint16_t))
		return false;
	memset(restore_state->cvq_state.vlan_bitmap, 0, sizeof(restore_state->cvq_state.vlan_bitmap));
	for (i = 0; i < entries; i++) {
		vid = le16toh(vlan->vlan[i]);
		if (vid >= VNET_VF_CVQ_MAX_VLAN_ENTRIES ||
		    (restore_state->cvq_state.vlan_bitmap[vid / 64] & (1ULL << (vid % 64))) != 0)
			return false;
		restore_state->cvq_state.vlan_bitmap[vid / 64] |= 1ULL << (vid % 64);
	}
	restore_state->cvq_state.vlan_entries = entries;
	return true;
}

static bool vnet_admin_dev_parts_cvq_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
					    const struct vnet_vf_slot *slot,
					    const struct vnet_admin_part_profile *part,
					    uint32_t value_len,
					    const uint8_t *value)
{
	switch (part->class) {
	case VIRTIO_NET_CTRL_MQ:
		return vnet_admin_dev_parts_cvq_mq_decode(restore_state, slot, value);
	case VIRTIO_NET_CTRL_MAC:
		return vnet_admin_dev_parts_cvq_mac_decode(restore_state, part, value_len, value);
	case VIRTIO_NET_CTRL_VLAN:
		if (part->command != VIRTIO_NET_CTRL_VLAN_ADD)
			return false;
		return vnet_admin_dev_parts_cvq_vlan_decode(restore_state, value_len, value);
	default:
		return false;
	}
}

static bool vnet_admin_dev_parts_value_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
					      struct vnet_vf_slot *slot,
					      const struct vnet_admin_part_profile *part,
					      uint32_t value_len,
					      const uint8_t *value)
{
	switch (part->type) {
	case VIRTIO_DEV_PART_DEV_FEATURES:
		return le64toh(((const struct virtio_dev_part_features *)value)->feature_bits[0]) ==
		       slot->vf_dev->device_features;
	case VIRTIO_DEV_PART_DRV_FEATURES:
		restore_state->driver_features =
			le64toh(((const struct virtio_dev_part_features *)value)->feature_bits[0]);
		return (restore_state->driver_features & ~slot->vf_dev->device_features) == 0;
	case VIRTIO_DEV_PART_PCI_COMMON_CFG:
		memcpy(&restore_state->common, value, sizeof(restore_state->common));
		return vnet_admin_dev_parts_common_validate(restore_state, slot, &restore_state->common);
	case VIRTIO_DEV_PART_DEVICE_STATUS:
		return vnet_admin_dev_parts_device_status_decode(restore_state, value);
	case VNET_ADMIN_DEV_PART_NET_CFG:
		return vnet_admin_dev_parts_net_config_decode(restore_state, slot, value);
	case VIRTIO_DEV_PART_VQ_CFG:
		return vnet_admin_dev_parts_vq_config_decode(restore_state, part, value);
	case VIRTIO_DEV_PART_VQ_NOTIFY_CFG:
		return vnet_admin_dev_parts_vq_notify_decode(restore_state, slot, part, value);
	case VIRTIO_NET_DEV_PART_CVQ_CFG_PART:
		return vnet_admin_dev_parts_cvq_decode(restore_state, slot, part, value_len, value);
	default:
		return false;
	}
}

static doca_error_t vnet_admin_dev_parts_restore_state_prepare(const struct vnet_vf_slot *slot,
							       const uint8_t *parts,
							       uint32_t parts_len,
							       struct vnet_admin_dev_parts_profile **profile,
							       struct vnet_admin_dev_parts_restore_state **restore_state)
{
	struct vnet_admin_dev_parts_restore_state *new_restore_state = NULL;
	struct vnet_admin_dev_parts_profile *new_profile = NULL;
	const struct vnet_pci_device *vf = slot->vf_dev;
	uint16_t *vq_indices = NULL;
	doca_error_t result = DOCA_ERROR_INVALID_VALUE;
	uint16_t num_vqs;

	vq_indices = calloc(slot->vf_dev->vqs_count, sizeof(*vq_indices));
	if (vq_indices == NULL)
		return DOCA_ERROR_NO_MEMORY;

	if (!vnet_admin_dev_parts_vq_cardinality_get(parts, parts_len, slot->vf_dev->vqs_count, vq_indices, &num_vqs))
		goto out;

	new_profile = vnet_admin_dev_parts_profile_create(vq_indices, num_vqs, 0, 0, 0);
	if (new_profile == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto out;
	}

	new_restore_state = calloc(1, sizeof(*new_restore_state));
	if (new_restore_state == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto out;
	}
	new_restore_state->vqs = calloc(slot->vf_dev->vqs_count, sizeof(*new_restore_state->vqs));
	if (new_restore_state->vqs == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto out;
	}

	/*
	 * A SET may contain any valid subset of device parts. Seed the candidate
	 * from the current stopped VF state so omitted parts remain unchanged.
	 */
	pthread_mutex_lock(&slot->vf_dev->vqs_mutex);
	vnet_admin_dev_parts_common_encode(vf, &new_restore_state->common);
	memset(&new_restore_state->net, 0, sizeof(new_restore_state->net));
	vnet_admin_dev_parts_net_encode(&vf->vnet_cfg, &new_restore_state->net);
	memcpy(new_restore_state->vqs, vf->vqs, vf->vqs_count * sizeof(*vf->vqs));
	memcpy(&new_restore_state->cvq_state, &slot->cvq_state, sizeof(new_restore_state->cvq_state));
	new_restore_state->driver_features = vf->driver_features;
	new_restore_state->active_qps = atomic_load(&slot->num_active_qps);
	new_restore_state->device_status = vf->pci_cfg.device_status;
	memcpy(new_restore_state->primary_mac, vf->vnet_cfg.mac, sizeof(new_restore_state->primary_mac));
	pthread_mutex_unlock(&slot->vf_dev->vqs_mutex);

	*profile = new_profile;
	*restore_state = new_restore_state;
	new_profile = NULL;
	new_restore_state = NULL;
	result = DOCA_SUCCESS;

out:
	if (new_restore_state != NULL)
		free(new_restore_state->vqs);
	free(new_restore_state);
	free(new_profile);
	free(vq_indices);
	return result;
}

static bool vnet_admin_dev_parts_restore_state_decode(struct vnet_admin_dev_parts_restore_state *restore_state,
						      struct vnet_vf_slot *slot,
						      const struct vnet_admin_dev_parts_profile *profile,
						      const uint8_t *parts,
						      uint32_t parts_len)
{
	const struct virtio_dev_part_hdr *hdr;
	const uint8_t *cursor = parts;
	uint32_t remaining = parts_len, profile_idx = 0, match_idx, value_len;

	while (remaining != 0) {
		if (remaining < sizeof(*hdr))
			return false;
		hdr = (const struct virtio_dev_part_hdr *)cursor;
		value_len = le32toh(hdr->length);
		if (value_len > remaining - sizeof(*hdr) || hdr->reserved != 0 ||
		    (hdr->flags & ~(1U << VIRTIO_DEV_PART_F_OPTIONAL)) != 0)
			return false;

		match_idx = profile_idx;
		while (match_idx < profile->count &&
		       !vnet_admin_dev_parts_header_matches(hdr, &profile->parts[match_idx]))
			match_idx++;
		if (match_idx >= profile->count) {
			if ((hdr->flags & (1U << VIRTIO_DEV_PART_F_OPTIONAL)) == 0 ||
			    vnet_admin_dev_parts_type_is_known(le16toh(hdr->part_type)) ||
			    !vnet_admin_dev_parts_bytes_zero(&hdr->selector, sizeof(hdr->selector)))
				return false;
			cursor += sizeof(*hdr) + value_len;
			remaining -= sizeof(*hdr) + value_len;
			continue;
		}

		profile_idx = match_idx;
		if (!vnet_admin_dev_parts_value_decode(restore_state,
						       slot,
						       &profile->parts[profile_idx],
						       value_len,
						       cursor + sizeof(*hdr)))
			return false;
		profile_idx++;
		cursor += sizeof(*hdr) + value_len;
		remaining -= sizeof(*hdr) + value_len;
	}

	return true;
}

doca_error_t vnet_admin_dev_parts_restore_state_create(struct vnet_vf_slot *slot,
						       const uint8_t *parts,
						       uint32_t parts_len,
						       struct vnet_admin_dev_parts_restore_state **restore_state)
{
	struct vnet_admin_dev_parts_restore_state *new_restore_state = NULL;
	struct vnet_admin_dev_parts_profile *profile = NULL;
	doca_error_t result = DOCA_ERROR_INVALID_VALUE;

	if (slot == NULL || slot->vf_dev == NULL || parts == NULL || restore_state == NULL || parts_len == 0)
		return DOCA_ERROR_INVALID_VALUE;
	*restore_state = NULL;
	if (parts_len > vnet_admin_dev_parts_max_payload_size(slot->vf_dev->vqs_count))
		return DOCA_ERROR_INVALID_VALUE;

	result = vnet_admin_dev_parts_restore_state_prepare(slot, parts, parts_len, &profile, &new_restore_state);
	if (result != DOCA_SUCCESS)
		goto out;

	if (!vnet_admin_dev_parts_restore_state_decode(new_restore_state, slot, profile, parts, parts_len))
		goto out;
	if (!vnet_admin_dev_parts_restore_state_validate(new_restore_state, slot))
		goto out;
	if (new_restore_state->cvq_state.mac_entries != 0 || new_restore_state->cvq_state.vlan_entries != 0) {
		DOCA_LOG_WARN("Device-parts SET retains non-empty MAC/VLAN filter tables as shadows "
			      "but cannot apply them to the backend");
	}

	*restore_state = new_restore_state;
	new_restore_state = NULL;
	result = DOCA_SUCCESS;

out:
	if (new_restore_state != NULL)
		free(new_restore_state->vqs);
	free(new_restore_state);
	free(profile);
	return result;
}

void vnet_admin_dev_parts_restore_state_destroy(struct vnet_admin_dev_parts_restore_state *restore_state)
{
	if (restore_state == NULL)
		return;
	free(restore_state->vqs);
	free(restore_state);
}

static bool vnet_admin_dev_parts_restore_state_is_identical_locked(
	const struct vnet_admin_dev_parts_restore_state *restore_state,
	const struct vnet_vf_slot *slot)
{
	const struct vnet_pci_device *vf = slot->vf_dev;
	bool restore_mq;
	bool identical;

	restore_mq = (restore_state->driver_features & (1ULL << VIRTIO_NET_F_CTRL_VQ)) != 0 &&
		     (restore_state->driver_features & (1ULL << VIRTIO_NET_F_MQ)) != 0;
	identical = vf->driver_features == restore_state->driver_features &&
		    vf->pci_cfg.device_feature_select == le32toh(restore_state->common.device_feature_select) &&
		    vf->pci_cfg.device_feature == le32toh(restore_state->common.device_feature) &&
		    vf->pci_cfg.driver_feature_select == le32toh(restore_state->common.driver_feature_select) &&
		    vf->pci_cfg.driver_feature == le32toh(restore_state->common.driver_feature) &&
		    vf->pci_cfg.config_msix_vector == le16toh(restore_state->common.config_msix_vector) &&
		    vf->pci_cfg.config_generation == restore_state->common.config_generation &&
		    vf->pci_cfg.queue_select == le16toh(restore_state->common.queue_select) &&
		    vf->pci_cfg.device_status == restore_state->device_status &&
		    vf->vnet_cfg.status == le16toh(restore_state->net.status) &&
		    vf->vnet_cfg.max_virtqueue_pairs == le16toh(restore_state->net.max_virtqueue_pairs) &&
		    vf->vnet_cfg.mtu == le16toh(restore_state->net.mtu) &&
		    memcmp(vf->vnet_cfg.mac, restore_state->primary_mac, ETH_ALEN) == 0 &&
		    memcmp(vf->vqs, restore_state->vqs, vf->vqs_count * sizeof(*vf->vqs)) == 0 &&
		    memcmp(&slot->cvq_state, &restore_state->cvq_state, sizeof(slot->cvq_state)) == 0 &&
		    atomic_load(&slot->num_active_qps) == restore_state->active_qps &&
		    slot->mq_feature_negotiated == restore_mq;
	return identical;
}

doca_error_t vnet_admin_dev_parts_restore_state_apply(struct vnet_admin_dev_parts_restore_state *restore_state,
						      struct vnet_vf_slot *slot,
						      uint64_t admin_generation,
						      bool *rebuild_required)
{
	struct vnet_pci_device *vf;
	bool new_mq;

	if (restore_state == NULL || slot == NULL || rebuild_required == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	vf = slot->vf_dev;
	if (vf == NULL)
		return DOCA_ERROR_BAD_STATE;

	pthread_mutex_lock(&vf->vqs_mutex);
	if (admin_generation != atomic_load(&slot->admin_operation_generation) ||
	    vnet_recycle_in_progress(&slot->recycle) || !atomic_load(&slot->active)) {
		pthread_mutex_unlock(&vf->vqs_mutex);
		return DOCA_ERROR_IN_PROGRESS;
	}
	if (vnet_admin_dev_parts_restore_state_is_identical_locked(restore_state, slot)) {
		*rebuild_required = atomic_load(&slot->admin_rebuild_required);
		pthread_mutex_unlock(&vf->vqs_mutex);
		return DOCA_SUCCESS;
	}

	new_mq = (restore_state->driver_features & (1ULL << VIRTIO_NET_F_CTRL_VQ)) != 0 &&
		 (restore_state->driver_features & (1ULL << VIRTIO_NET_F_MQ)) != 0;
	*rebuild_required = true;

	vf->driver_features = restore_state->driver_features;
	vf->pci_cfg.device_feature_select = le32toh(restore_state->common.device_feature_select);
	vf->pci_cfg.device_feature = le32toh(restore_state->common.device_feature);
	vf->pci_cfg.driver_feature_select = le32toh(restore_state->common.driver_feature_select);
	vf->pci_cfg.driver_feature = le32toh(restore_state->common.driver_feature);
	vf->pci_cfg.config_msix_vector = le16toh(restore_state->common.config_msix_vector);
	vf->pci_cfg.config_generation = restore_state->common.config_generation;
	vf->pci_cfg.queue_select = le16toh(restore_state->common.queue_select);
	vf->pci_cfg.device_status = restore_state->device_status;
	vf->vnet_cfg.status = le16toh(restore_state->net.status);
	vf->vnet_cfg.max_virtqueue_pairs = le16toh(restore_state->net.max_virtqueue_pairs);
	vf->vnet_cfg.mtu = le16toh(restore_state->net.mtu);
	memcpy(vf->vqs, restore_state->vqs, vf->vqs_count * sizeof(*vf->vqs));
	if (vf->pci_cfg.queue_select < vf->vqs_count)
		memcpy(&vf->pci_cfg.vq, &vf->vqs[vf->pci_cfg.queue_select], sizeof(vf->pci_cfg.vq));
	else
		memset(&vf->pci_cfg.vq, 0, sizeof(vf->pci_cfg.vq));
	atomic_store(&slot->num_active_qps, restore_state->active_qps);
	memcpy(vf->vnet_cfg.mac, restore_state->primary_mac, ETH_ALEN);
	memcpy(&slot->cvq_state, &restore_state->cvq_state, sizeof(slot->cvq_state));
	slot->mq_feature_negotiated = new_mq;
	atomic_store(&slot->admin_rebuild_required, true);
	pthread_mutex_unlock(&vf->vqs_mutex);

	vnet_admin_dev_parts_invalidate(slot);
	return DOCA_SUCCESS;
}

void vnet_admin_dev_parts_invalidate(struct vnet_vf_slot *slot)
{
	if (slot != NULL)
		atomic_fetch_add(&slot->admin_cache_generation, 1);
}

void vnet_admin_dev_parts_cvq_reset(struct vnet_vf_slot *slot)
{
	if (slot == NULL || slot->vf_dev == NULL)
		return;

	pthread_mutex_lock(&slot->vf_dev->vqs_mutex);
	memset(&slot->cvq_state, 0, sizeof(slot->cvq_state));
	pthread_mutex_unlock(&slot->vf_dev->vqs_mutex);

	vnet_admin_dev_parts_invalidate(slot);
}

void vnet_admin_dev_parts_metadata_get(struct vnet_admin_resource_obj *obj,
				       struct vnet_vf_slot *slot,
				       const struct vnet_admin_cmd_view *view,
				       uint8_t metadata_type,
				       struct vnet_admin_cmd_result *result)
{
	uint8_t *wire;
	enum vnet_admin_capture_status status;
	uint32_t wire_len = 0, attempt;

	if (metadata_type > VIRTIO_ADMIN_CMD_DEV_PARTS_METADATA_TYPE_LIST) {
		DOCA_LOG_ERR("Unsupported device-parts metadata type %u", metadata_type);
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}
	for (attempt = 0; attempt < VNET_ADMIN_CAPTURE_RETRY_LIMIT; attempt++) {
		status = vnet_admin_dev_parts_profile_ensure(obj, slot);
		if (status != VNET_ADMIN_CAPTURE_OK) {
			if (status == VNET_ADMIN_CAPTURE_NORESOURCE)
				DOCA_LOG_ERR("Failed to allocate device-parts metadata snapshot");
			vnet_admin_dev_parts_result_set(result,
							status == VNET_ADMIN_CAPTURE_NORESOURCE ?
								VIRTIO_ADMIN_STATUS_Q_NORESOURCE :
								VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
			return;
		}
		wire = vnet_admin_dev_parts_metadata_result_create(obj->profile, metadata_type, &wire_len);
		if (wire == NULL) {
			DOCA_LOG_ERR("Failed to allocate device-parts metadata result");
			vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
			return;
		}
		if (obj->profile->generation == atomic_load(&slot->admin_cache_generation))
			break;
		free(wire);
	}
	if (attempt == VNET_ADMIN_CAPTURE_RETRY_LIMIT) {
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
		return;
	}

	if (!vnet_admin_cmd_view_has_result(view) || view->result_len < wire_len) {
		DOCA_LOG_ERR("Device-parts metadata result is too small (have=%u, need=%u)",
			     view->result_len,
			     wire_len);
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
	} else if (!vnet_admin_cmd_view_write(view, wire, wire_len)) {
		DOCA_LOG_ERR("Failed to write device-parts metadata result");
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
	} else {
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
		result->len = wire_len;
	}
	free(wire);
}

void vnet_admin_dev_parts_get(struct vnet_admin_resource_obj *obj,
			      struct vnet_vf_slot *slot,
			      const struct vnet_admin_cmd_view *view,
			      struct vnet_admin_cmd_result *result)
{
	uint64_t generation;
	enum vnet_admin_capture_status status;

	generation = atomic_load(&slot->admin_cache_generation);
	if (obj->cache == NULL || obj->profile == NULL || obj->cache_generation != generation ||
	    obj->profile->generation != generation) {
		status = vnet_admin_dev_parts_cache_create(obj, slot);
		if (status != VNET_ADMIN_CAPTURE_OK) {
			if (status == VNET_ADMIN_CAPTURE_NORESOURCE)
				DOCA_LOG_ERR("Failed to allocate device-parts GET snapshot");
			else
				DOCA_LOG_ERR("Failed to capture VF device parts");
			vnet_admin_dev_parts_result_set(result,
							status == VNET_ADMIN_CAPTURE_NORESOURCE ?
								VIRTIO_ADMIN_STATUS_Q_NORESOURCE :
								VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
			return;
		}
	}

	if (!vnet_admin_cmd_view_has_result(view) || view->result_len < obj->cache_len) {
		DOCA_LOG_ERR("Device-parts GET result is too small (have=%u, need=%u)",
			     view->result_len,
			     obj->cache_len);
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}
	if (!vnet_admin_cmd_view_write(view, obj->cache, obj->cache_len)) {
		DOCA_LOG_ERR("Failed to write device-parts GET result");
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		return;
	}

	vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
	result->len = obj->cache_len;
}

void vnet_admin_dev_parts_get_selected(struct vnet_admin_resource_obj *obj,
				       struct vnet_vf_slot *slot,
				       const struct virtio_dev_part_hdr *hdrs,
				       uint32_t hdr_count,
				       const struct vnet_admin_cmd_view *view,
				       struct vnet_admin_cmd_result *result)
{
	struct virtio_dev_part_hdr expected = {0};
	uint8_t *selected = NULL;
	enum vnet_admin_capture_status status;
	uint64_t generation;
	uint32_t cache_offset = 0, result_len = 0, profile_idx = 0;
	uint32_t record_len, i;

	generation = atomic_load(&slot->admin_cache_generation);
	if (obj->cache == NULL || obj->profile == NULL || obj->cache_generation != generation ||
	    obj->profile->generation != generation) {
		status = vnet_admin_dev_parts_cache_create(obj, slot);
		if (status != VNET_ADMIN_CAPTURE_OK) {
			if (status == VNET_ADMIN_CAPTURE_NORESOURCE)
				DOCA_LOG_ERR("Failed to allocate device-parts GET SELECTED snapshot");
			else
				DOCA_LOG_ERR("Failed to capture VF device parts");
			vnet_admin_dev_parts_result_set(result,
							status == VNET_ADMIN_CAPTURE_NORESOURCE ?
								VIRTIO_ADMIN_STATUS_Q_NORESOURCE :
								VIRTIO_ADMIN_STATUS_Q_TRYAGAIN);
			return;
		}
	}

	selected = malloc(obj->cache_len);
	if (selected == NULL) {
		DOCA_LOG_ERR("Failed to allocate device-parts GET SELECTED result");
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		return;
	}

	for (i = 0; i < hdr_count; i++) {
		while (profile_idx < obj->profile->count) {
			vnet_admin_dev_parts_hdr_encode(&obj->profile->parts[profile_idx], &expected);
			if (memcmp(&expected, &hdrs[i], sizeof(expected)) == 0)
				break;
			cache_offset += sizeof(expected) + obj->profile->parts[profile_idx].value_len;
			profile_idx++;
		}
		if (profile_idx == obj->profile->count) {
			DOCA_LOG_ERR("DEV_PARTS_GET SELECTED header %u is unknown or out of order", i);
			vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
			goto out;
		}

		record_len = sizeof(expected) + obj->profile->parts[profile_idx].value_len;
		memcpy(selected + result_len, obj->cache + cache_offset, record_len);
		result_len += record_len;
		cache_offset += record_len;
		profile_idx++;
	}

	if (!vnet_admin_cmd_view_has_result(view) || view->result_len < result_len) {
		DOCA_LOG_ERR("Device-parts GET SELECTED result is too small (have=%u, need=%u)",
			     view->result_len,
			     result_len);
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_NORESOURCE);
		goto out;
	}
	if (!vnet_admin_cmd_view_write(view, selected, result_len)) {
		DOCA_LOG_ERR("Failed to write device-parts GET SELECTED result");
		vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_INVALID_FIELD);
		goto out;
	}

	vnet_admin_dev_parts_result_set(result, VIRTIO_ADMIN_STATUS_Q_OK);
	result->len = result_len;
out:
	free(selected);
}
