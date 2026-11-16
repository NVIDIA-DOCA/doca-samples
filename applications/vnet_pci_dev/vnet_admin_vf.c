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
#include <unistd.h>
#include <linux/if_ether.h>

#include <doca_buf.h>
#include <doca_devemu_vnet.h>
#include <doca_devemu_vnet_counters.h>
#include <doca_devemu_vnet_io.h>
#include <doca_devemu_vnet_offload_engine.h>
#include <doca_devemu_virtio.h>
#include <doca_devemu_virtio_io.h>
#include <doca_devemu_virtio_tlp.h>
#include <doca_log.h>

#include "vnet_admin_dev_parts.h"
#include "vnet_admin_vf.h"
#include "vnet_pci_dev_core.h"
#include "vnet_pci_device.h"

DOCA_LOG_REGISTER(VNET_ADMIN_VF);

#define VNET_ADMIN_VF_DISABLE_POLL_USEC 1000
#define VNET_ADMIN_VF_DISABLE_POLL_MAX 200
#define VNET_ADMIN_VF_MAC_TABLE_COUNT 2U

static bool vnet_admin_vf_generation_valid(const struct vnet_vf_slot *slot, uint64_t generation)
{
	return generation == atomic_load(&slot->admin_operation_generation) &&
	       !vnet_recycle_in_progress(&slot->recycle) && atomic_load(&slot->active);
}

static void vnet_admin_vf_reset_operation(struct vnet_vf_slot *slot)
{
	atomic_fetch_add(&slot->admin_operation_generation, 1);
	atomic_store(&slot->admin_stop_requested, false);
	atomic_store(&slot->admin_stopped, false);
}

static doca_error_t vnet_admin_vf_engine_disable_and_drain(struct vnet_vf_slot *slot,
							   struct doca_devemu_virtio_offload_engine *engine,
							   struct doca_pe *worker_pe)
{
	enum doca_devemu_virtio_offload_engine_states state;
	doca_error_t result;
	int i;

	if (worker_pe == NULL)
		return DOCA_ERROR_BAD_STATE;

	result = doca_devemu_virtio_offload_engine_get_state(engine, &state);
	if (result != DOCA_SUCCESS)
		return result;
	if (state == DOCA_DEVEMU_VIRTIO_OFFLOAD_ENGINE_STATE_ENABLED) {
		result = doca_devemu_virtio_offload_engine_disable(engine);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS)
			return result;
	} else if (state == DOCA_DEVEMU_VIRTIO_OFFLOAD_ENGINE_STATE_DISABLED) {
		return DOCA_SUCCESS;
	}

	for (i = 0; i < VNET_ADMIN_VF_DISABLE_POLL_MAX; i++) {
		(void)doca_pe_progress(worker_pe);
		result = doca_devemu_virtio_offload_engine_get_state(engine, &state);
		if (result != DOCA_SUCCESS)
			return result;
		if (state == DOCA_DEVEMU_VIRTIO_OFFLOAD_ENGINE_STATE_DISABLED)
			return DOCA_SUCCESS;
		usleep(VNET_ADMIN_VF_DISABLE_POLL_USEC);
	}

	DOCA_LOG_WARN("SR-IOV: VF[vf_index=%u] engine disable timed out", slot->vf_index);
	return DOCA_ERROR_IN_PROGRESS;
}

static doca_error_t vnet_admin_vf_configure_stopped_vqs(struct vnet_vf_slot *slot)
{
	struct vnet_virtio_queue_config cvq_cfg = {0};
	struct doca_devemu_virtio_vq *vq;
	doca_error_t result;
	uint16_t active_qps, i;

	active_qps = atomic_load(&slot->num_active_qps);
	if ((slot->vf_dev->pci_cfg.device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK) != 0 &&
	    (active_qps == 0 || active_qps > slot->max_queue_pairs))
		return DOCA_ERROR_BAD_STATE;

	for (i = 0; i < active_qps * 2; i++) {
		result = vnet_pci_dev_vf_set_conf_data_vq(slot, i, &vq, NULL);
		if (result != DOCA_SUCCESS)
			return result;
	}

	if (!slot->mq_feature_negotiated || slot->cvq == NULL)
		return DOCA_SUCCESS;
	i = VNET_CVQ_INDEX(slot->max_queue_pairs);
	if (!vnet_pci_device_snapshot_vq(slot->vf_dev, i, &cvq_cfg) || !cvq_cfg.queue_enable ||
	    cvq_cfg.queue_desc == 0 || cvq_cfg.queue_driver == 0 || cvq_cfg.queue_device == 0)
		return DOCA_SUCCESS;
	vq = doca_devemu_vnet_ctrl_vq_as_vq(slot->cvq);
	if (vq == NULL)
		return DOCA_ERROR_UNEXPECTED;
	return doca_devemu_virtio_vq_set_conf(vq,
					      i,
					      cvq_cfg.queue_size,
					      cvq_cfg.queue_msix_vector,
					      cvq_cfg.queue_desc,
					      cvq_cfg.queue_driver,
					      cvq_cfg.queue_device);
}

static bool vnet_admin_vf_ctrl_data_copy(struct doca_buf *buf, void *data, uint32_t len)
{
	struct doca_buf *curr = buf;
	doca_error_t result;
	uint8_t *dst = data;
	size_t curr_len;
	void *curr_data;
	uint32_t processed = 0, chunk;

	while (curr != NULL && processed < len) {
		result = doca_buf_get_data_len(curr, &curr_len);
		if (result != DOCA_SUCCESS)
			break;
		result = doca_buf_get_data(curr, &curr_data);
		if (result != DOCA_SUCCESS || curr_data == NULL)
			break;
		chunk = len - processed < curr_len ? len - processed : (uint32_t)curr_len;
		memcpy(dst + processed, curr_data, chunk);
		processed += chunk;
		if (processed < len && doca_buf_get_next_in_list(curr, &curr) != DOCA_SUCCESS)
			break;
	}

	return processed == len;
}

static bool vnet_admin_vf_ctrl_mac_table_capture(struct vnet_vf_slot *slot,
						 struct doca_buf *data_buf,
						 uint32_t data_len)
{
	uint8_t macs[VNET_VF_CVQ_MAX_MAC_ENTRIES][ETH_ALEN] = {0};
	uint32_t total_entries = 0, offset = 0, entries, table;
	uint8_t *wire = NULL;
	bool valid = false;

	if (data_buf == NULL || data_len < VNET_ADMIN_VF_MAC_TABLE_COUNT * sizeof(entries))
		return false;
	wire = calloc(1, data_len);
	if (wire == NULL || !vnet_admin_vf_ctrl_data_copy(data_buf, wire, data_len))
		goto out;

	for (table = 0; table < VNET_ADMIN_VF_MAC_TABLE_COUNT; table++) {
		if (data_len - offset < sizeof(entries))
			goto out;
		memcpy(&entries, wire + offset, sizeof(entries));
		entries = le32toh(entries);
		offset += sizeof(entries);
		if (entries > (data_len - offset) / ETH_ALEN || entries > VNET_VF_CVQ_MAX_MAC_ENTRIES - total_entries)
			goto out;
		memcpy(&macs[total_entries], wire + offset, entries * ETH_ALEN);
		total_entries += entries;
		offset += entries * ETH_ALEN;
	}
	if (offset != data_len)
		goto out;

	pthread_mutex_lock(&slot->vf_dev->vqs_mutex);
	slot->cvq_state.mac_entries = total_entries;
	memset(slot->cvq_state.macs, 0, sizeof(slot->cvq_state.macs));
	memcpy(slot->cvq_state.macs, macs, total_entries * ETH_ALEN);
	pthread_mutex_unlock(&slot->vf_dev->vqs_mutex);

	vnet_admin_dev_parts_invalidate(slot);
	valid = true;

out:
	free(wire);
	return valid;
}

static bool vnet_admin_vf_ctrl_primary_mac_capture(struct vnet_vf_slot *slot,
						   struct doca_buf *data_buf,
						   uint32_t data_len)
{
	uint8_t mac[ETH_ALEN] = {0};

	if (data_buf == NULL || data_len != sizeof(mac) || !vnet_admin_vf_ctrl_data_copy(data_buf, mac, sizeof(mac)))
		return false;

	pthread_mutex_lock(&slot->vf_dev->vqs_mutex);
	memcpy(slot->vf_dev->vnet_cfg.mac, mac, sizeof(mac));
	pthread_mutex_unlock(&slot->vf_dev->vqs_mutex);

	vnet_admin_dev_parts_invalidate(slot);
	return true;
}

static bool vnet_admin_vf_ctrl_vlan_capture(struct vnet_vf_slot *slot,
					    struct doca_buf *data_buf,
					    uint32_t data_len,
					    uint8_t cmd)
{
	uint16_t vlan;
	uint64_t mask;
	bool present;

	if (cmd != VIRTIO_NET_CTRL_VLAN_ADD && cmd != VIRTIO_NET_CTRL_VLAN_DEL)
		return false;
	if (data_buf == NULL || data_len != sizeof(vlan) ||
	    !vnet_admin_vf_ctrl_data_copy(data_buf, &vlan, sizeof(vlan)))
		return false;
	vlan = le16toh(vlan);
	if (vlan >= VNET_VF_CVQ_MAX_VLAN_ENTRIES)
		return false;

	mask = 1ULL << (vlan % 64);
	pthread_mutex_lock(&slot->vf_dev->vqs_mutex);
	present = (slot->cvq_state.vlan_bitmap[vlan / 64] & mask) != 0;
	if (cmd == VIRTIO_NET_CTRL_VLAN_ADD && !present) {
		slot->cvq_state.vlan_bitmap[vlan / 64] |= mask;
		slot->cvq_state.vlan_entries++;
	} else if (cmd == VIRTIO_NET_CTRL_VLAN_DEL && present) {
		slot->cvq_state.vlan_bitmap[vlan / 64] &= ~mask;
		slot->cvq_state.vlan_entries--;
	}
	pthread_mutex_unlock(&slot->vf_dev->vqs_mutex);

	vnet_admin_dev_parts_invalidate(slot);
	return true;
}

static bool vnet_admin_vf_ctrl_steering_capture(struct vnet_vf_slot *slot,
						struct doca_devemu_vnet_ctrl_req *req,
						uint8_t cls,
						uint8_t cmd)
{
	struct doca_buf *data_buf = doca_devemu_vnet_ctrl_req_get_data(req);
	uint32_t data_len = doca_devemu_vnet_ctrl_req_get_data_len(req);

	if (cls == VIRTIO_NET_CTRL_MAC && cmd == VIRTIO_NET_CTRL_MAC_TABLE_SET)
		return vnet_admin_vf_ctrl_mac_table_capture(slot, data_buf, data_len);
	if (cls == VIRTIO_NET_CTRL_MAC && cmd == VIRTIO_NET_CTRL_MAC_ADDR_SET)
		return vnet_admin_vf_ctrl_primary_mac_capture(slot, data_buf, data_len);
	if (cls == VIRTIO_NET_CTRL_VLAN)
		return vnet_admin_vf_ctrl_vlan_capture(slot, data_buf, data_len, cmd);
	return false;
}

static doca_error_t vnet_admin_vf_start_added_queue_pairs(struct vnet_vf_slot *slot, uint16_t old_qps, uint16_t new_qps)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	struct doca_devemu_virtio_vq *vq;
	doca_error_t result;
	uint16_t vq_index, qp;

	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(slot->offload_engine);
	if (virtio_engine == NULL)
		return DOCA_ERROR_UNEXPECTED;

	for (qp = old_qps; qp < new_qps; qp++) {
		for (vq_index = (uint16_t)(qp * 2); vq_index < (uint16_t)(qp * 2 + 2); vq_index++) {
			result = vnet_pci_dev_vf_set_conf_data_vq(slot, vq_index, &vq, NULL);
			if (result != DOCA_SUCCESS || vq == NULL)
				return result != DOCA_SUCCESS ? result : DOCA_ERROR_BAD_STATE;
			result = doca_devemu_virtio_vq_start(vq);
			if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
				return result;
		}
	}

	return doca_devemu_virtio_vq_group_enable(virtio_engine, (uint16_t)(old_qps * 2), (uint16_t)(new_qps * 2 - 1));
}

static doca_error_t vnet_admin_vf_stop_removed_queue_pairs(struct vnet_vf_slot *slot,
							   uint16_t old_qps,
							   uint16_t new_qps)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	struct doca_devemu_virtio_vq *vq;
	doca_error_t result;
	uint16_t qp;

	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(slot->offload_engine);
	if (virtio_engine == NULL)
		return DOCA_ERROR_UNEXPECTED;

	result = doca_devemu_virtio_vq_group_disable(virtio_engine,
						     (uint16_t)(new_qps * 2),
						     (uint16_t)(old_qps * 2 - 1));
	if (result != DOCA_SUCCESS)
		return result;

	for (qp = new_qps; qp < old_qps; qp++) {
		if (slot->rx_vqs[qp] == NULL || slot->tx_vqs[qp] == NULL)
			return DOCA_ERROR_BAD_STATE;

		vq = doca_devemu_vnet_rx_vq_as_vq(slot->rx_vqs[qp]);
		result = doca_devemu_virtio_vq_stop(vq);
		if (result != DOCA_SUCCESS)
			return result;

		vq = doca_devemu_vnet_tx_vq_as_vq(slot->tx_vqs[qp]);
		result = doca_devemu_virtio_vq_stop(vq);
		if (result != DOCA_SUCCESS)
			return result;
	}
	return DOCA_SUCCESS;
}

static uint8_t vnet_admin_vf_ctrl_mq_handle(struct vnet_vf_slot *slot,
					    struct doca_devemu_vnet_ctrl_req *req,
					    uint8_t cmd)
{
	struct doca_buf *data_buf;
	doca_error_t result;
	uint32_t data_len;
	uint16_t old_qps, new_qps;

	if (cmd != VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET) {
		DOCA_LOG_WARN("SR-IOV: VF[vf_index=%u] unsupported MQ cmd %u", slot->vf_index, cmd);
		return VIRTIO_NET_ERR;
	}

	data_buf = doca_devemu_vnet_ctrl_req_get_data(req);
	data_len = doca_devemu_vnet_ctrl_req_get_data_len(req);
	old_qps = atomic_load(&slot->num_active_qps);
	new_qps = 0;
	if (data_len == sizeof(new_qps) && data_buf != NULL &&
	    vnet_admin_vf_ctrl_data_copy(data_buf, &new_qps, sizeof(new_qps)))
		new_qps = le16toh(new_qps);

	if (new_qps == 0 || new_qps > slot->max_queue_pairs) {
		DOCA_LOG_WARN("SR-IOV: VF[vf_index=%u] MQ_VQ_PAIRS_SET %u rejected (max=%u)",
			      slot->vf_index,
			      new_qps,
			      slot->max_queue_pairs);
		return VIRTIO_NET_ERR;
	}
	if (new_qps > old_qps)
		result = vnet_admin_vf_start_added_queue_pairs(slot, old_qps, new_qps);
	else if (new_qps < old_qps)
		result = vnet_admin_vf_stop_removed_queue_pairs(slot, old_qps, new_qps);
	else
		result = DOCA_SUCCESS;
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("SR-IOV: VF[vf_index=%u] MQ resize %u -> %u failed: %s",
			      slot->vf_index,
			      old_qps,
			      new_qps,
			      doca_error_get_descr(result));
		return VIRTIO_NET_ERR;
	}

	atomic_store(&slot->num_active_qps, new_qps);
	vnet_admin_dev_parts_invalidate(slot);
	DOCA_LOG_INFO("SR-IOV: VF[vf_index=%u] MQ_VQ_PAIRS_SET %u", slot->vf_index, new_qps);
	return VIRTIO_NET_OK;
}

bool vnet_admin_vf_features_valid(uint64_t features)
{
	if ((features & (1ULL << VIRTIO_NET_F_CTRL_GUEST_OFFLOADS)) && !(features & (1ULL << VIRTIO_NET_F_CTRL_VQ))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_CTRL_GUEST_OFFLOADS requires VIRTIO_NET_F_CTRL_VQ");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_MQ)) && !(features & (1ULL << VIRTIO_NET_F_CTRL_VQ))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_MQ requires VIRTIO_NET_F_CTRL_VQ");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_CTRL_RX)) && !(features & (1ULL << VIRTIO_NET_F_CTRL_VQ))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_CTRL_RX requires VIRTIO_NET_F_CTRL_VQ");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_CTRL_VLAN)) && !(features & (1ULL << VIRTIO_NET_F_CTRL_VQ))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_CTRL_VLAN requires VIRTIO_NET_F_CTRL_VQ");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_GUEST_TSO4)) && !(features & (1ULL << VIRTIO_NET_F_GUEST_CSUM))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_GUEST_TSO4 requires VIRTIO_NET_F_GUEST_CSUM");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_GUEST_TSO6)) && !(features & (1ULL << VIRTIO_NET_F_GUEST_CSUM))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_GUEST_TSO6 requires VIRTIO_NET_F_GUEST_CSUM");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_GUEST_UFO)) && !(features & (1ULL << VIRTIO_NET_F_GUEST_CSUM))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_GUEST_UFO requires VIRTIO_NET_F_GUEST_CSUM");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_HOST_TSO4)) && !(features & (1ULL << VIRTIO_NET_F_CSUM))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_HOST_TSO4 requires VIRTIO_NET_F_CSUM");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_HOST_TSO6)) && !(features & (1ULL << VIRTIO_NET_F_CSUM))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_HOST_TSO6 requires VIRTIO_NET_F_CSUM");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_HOST_UFO)) && !(features & (1ULL << VIRTIO_NET_F_CSUM))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_HOST_UFO requires VIRTIO_NET_F_CSUM");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_GUEST_ECN)) &&
	    !((features & (1ULL << VIRTIO_NET_F_GUEST_TSO4)) || (features & (1ULL << VIRTIO_NET_F_GUEST_TSO6)))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_GUEST_ECN requires VIRTIO_NET_F_GUEST_TSO4 or VIRTIO_NET_F_GUEST_TSO6");
		return false;
	}
	if ((features & (1ULL << VIRTIO_NET_F_HOST_ECN)) &&
	    !((features & (1ULL << VIRTIO_NET_F_HOST_TSO4)) || (features & (1ULL << VIRTIO_NET_F_HOST_TSO6)))) {
		DOCA_LOG_ERR("VIRTIO_NET_F_HOST_ECN requires VIRTIO_NET_F_HOST_TSO4 or VIRTIO_NET_F_HOST_TSO6");
		return false;
	}

	return true;
}

void vnet_admin_vf_reset_config(struct vnet_vf_slot *slot)
{
	vnet_admin_vf_reset_operation(slot);
	vnet_admin_dev_parts_cvq_reset(slot);
	/* virtio-net comes out of reset using one queue pair until the guest asks
	 * for more, so a count the last session negotiated must not survive it.
	 * Zero, so the bring-up applies the default where it already does. */
	atomic_store(&slot->num_active_qps, 0);
}

bool vnet_admin_vf_config_write_begin(struct vnet_vf_slot *slot, bool reset)
{
	pthread_rwlock_wrlock(&slot->admin_config_lock);
	if ((atomic_load(&slot->admin_stop_requested) || atomic_load(&slot->admin_stopped) ||
	     atomic_load(&slot->admin_mutating)) &&
	    !reset) {
		pthread_rwlock_unlock(&slot->admin_config_lock);
		return false;
	}
	return true;
}

void vnet_admin_vf_config_write_end(struct vnet_vf_slot *slot)
{
	pthread_rwlock_unlock(&slot->admin_config_lock);
}

doca_error_t vnet_admin_vf_stop_datapath(struct vnet_vf_slot *slot, struct doca_pe *worker_pe)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	enum doca_devemu_virtio_offload_engine_states state;
	doca_error_t result;

	if (slot == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (slot->offload_engine == NULL) {
		if (atomic_load(&slot->engine_enabled) || atomic_load(&slot->vqs_created))
			return DOCA_ERROR_BAD_STATE;
		return DOCA_SUCCESS;
	}

	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(slot->offload_engine);
	if (virtio_engine == NULL)
		return DOCA_ERROR_UNEXPECTED;

	if (slot->vnet_counters != NULL) {
		result = doca_devemu_vnet_counters_destroy(slot->vnet_counters);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] counters destroy before admin stop failed: %s",
				     slot->vf_index,
				     doca_error_get_descr(result));
			return result;
		}
		slot->vnet_counters = NULL;
	}

	vnet_pci_dev_vf_disable_vqs(slot);
	result = vnet_admin_vf_engine_disable_and_drain(slot, virtio_engine, worker_pe);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_devemu_virtio_offload_engine_get_state(virtio_engine, &state);
	if (result != DOCA_SUCCESS)
		return result;
	if (state != DOCA_DEVEMU_VIRTIO_OFFLOAD_ENGINE_STATE_DISABLED)
		return DOCA_ERROR_IN_PROGRESS;

	atomic_store(&slot->engine_enabled, false);
	return vnet_pci_dev_vf_stop_vqs_checked(slot);
}

static doca_error_t vnet_admin_vf_rebuild_stopped_validate(struct vnet_vf_slot *slot, uint64_t generation)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	enum doca_devemu_virtio_offload_engine_states state;
	doca_error_t result;

	if (!atomic_load(&slot->admin_stopped) || !atomic_load(&slot->admin_stop_requested) ||
	    !atomic_load(&slot->admin_mutating)) {
		DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild stopped-state ownership check failed", slot->vf_index);
		return DOCA_ERROR_BAD_STATE;
	}
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild generation check failed", slot->vf_index);
		return DOCA_ERROR_IN_PROGRESS;
	}
	if (atomic_load(&slot->engine_enabled) || atomic_load(&slot->cvq_bound)) {
		DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild datapath is still enabled or bound", slot->vf_index);
		return DOCA_ERROR_BAD_STATE;
	}
	if (slot->offload_engine == NULL) {
		if (atomic_load(&slot->engine_started) || atomic_load(&slot->vqs_created)) {
			DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild engine/VQ ownership is inconsistent",
				     slot->vf_index);
			return DOCA_ERROR_BAD_STATE;
		}
		return DOCA_SUCCESS;
	}

	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(slot->offload_engine);
	if (virtio_engine == NULL) {
		DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild engine conversion failed", slot->vf_index);
		return DOCA_ERROR_UNEXPECTED;
	}
	result = doca_devemu_virtio_offload_engine_get_state(virtio_engine, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild engine state query failed: %s",
			     slot->vf_index,
			     doca_error_get_descr(result));
		return result;
	}
	if (state != DOCA_DEVEMU_VIRTIO_OFFLOAD_ENGINE_STATE_DISABLED) {
		DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild engine is not disabled", slot->vf_index);
		return DOCA_ERROR_BAD_STATE;
	}
	return DOCA_SUCCESS;
}

doca_error_t vnet_admin_vf_rebuild_stopped(struct tlp_context *tlp_ctx,
					   struct pci_device_config *pf,
					   struct vnet_vf_slot *slot,
					   uint64_t generation)
{
	struct doca_devemu_virtio_offload_engine *virtio_engine;
	const char *stage = "validate stopped state";
	doca_error_t result;

	if (tlp_ctx == NULL || pf == NULL || slot == NULL)
		return DOCA_ERROR_INVALID_VALUE;

	result = vnet_admin_vf_rebuild_stopped_validate(slot, generation);
	if (result != DOCA_SUCCESS)
		return result;

	/* Fence every stage so reset cannot leave resources owned by an expired admin operation. */
	stage = "destroy old engine";
	result = vnet_pci_dev_vf_engine_destroy(slot);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild failed at %s: %s",
			     slot->vf_index,
			     stage,
			     doca_error_get_descr(result));
		return result;
	}

	stage = "post-destroy generation check";
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		result = DOCA_ERROR_IN_PROGRESS;
		goto fail;
	}

	stage = "create engine";
	result = vnet_pci_dev_vf_engine_create(tlp_ctx, slot);
	if (result != DOCA_SUCCESS)
		goto fail;

	stage = "post-create generation check";
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		result = DOCA_ERROR_IN_PROGRESS;
		goto fail;
	}

	stage = "convert engine";
	virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(slot->offload_engine);
	if (virtio_engine == NULL) {
		result = DOCA_ERROR_UNEXPECTED;
		goto fail;
	}

	stage = "apply negotiated features";
	result = vnet_pci_dev_apply_negotiated_features(virtio_engine, slot->vf_dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	stage = "post-feature generation check";
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		result = DOCA_ERROR_IN_PROGRESS;
		goto fail;
	}

	stage = "start engine";
	result = doca_devemu_virtio_offload_engine_start(virtio_engine);
	if (result != DOCA_SUCCESS)
		goto fail;
	atomic_store(&slot->engine_started, true);

	stage = "post-start generation check";
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		result = DOCA_ERROR_IN_PROGRESS;
		goto fail;
	}

	stage = "create VQs";
	result = vnet_pci_dev_vf_vqs_create(tlp_ctx, slot);
	if (result != DOCA_SUCCESS)
		goto fail;

	stage = "post-VQ-create generation check";
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		result = DOCA_ERROR_IN_PROGRESS;
		goto fail;
	}

	stage = "initialize IO context";
	result = vnet_pci_dev_vf_initialize_io_context(tlp_ctx, pf, slot);
	if (result != DOCA_SUCCESS)
		goto fail;

	stage = "post-IO-context generation check";
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		result = DOCA_ERROR_IN_PROGRESS;
		goto fail;
	}

	/* Apply restored VQ configuration; newly created VQs remain disabled until DEV_MODE_SET(ACTIVE). */
	stage = "configure stopped VQs";
	result = vnet_admin_vf_configure_stopped_vqs(slot);
	if (result != DOCA_SUCCESS)
		goto fail;

	stage = "post-VQ-config generation check";
	if (!vnet_admin_vf_generation_valid(slot, generation)) {
		result = DOCA_ERROR_IN_PROGRESS;
		goto fail;
	}

	atomic_store(&slot->engine_enabled, false);
	return DOCA_SUCCESS;

fail:
	DOCA_LOG_ERR("SR-IOV: VF[vf_index=%u] rebuild failed at %s: %s",
		     slot->vf_index,
		     stage,
		     doca_error_get_descr(result));
	(void)vnet_pci_dev_vf_engine_destroy(slot);
	atomic_store(&slot->engine_enabled, false);
	return result;
}

void vnet_admin_vf_ctrl_req_handler(struct doca_devemu_vnet_ctrl_req *req, uint8_t cls, uint8_t cmd, void *user_data)
{
	struct vnet_vf_slot *slot;
	uint8_t ack = VIRTIO_NET_OK;

	(void)user_data;
	slot = (struct vnet_vf_slot *)doca_devemu_vnet_ctrl_req_get_vq_user_data(req);
	if (slot == NULL) {
		DOCA_LOG_ERR("SR-IOV: VF ctrl_req with no slot user data");
		doca_devemu_vnet_ctrl_req_complete(req, VIRTIO_NET_ERR, 0);
		return;
	}

	switch (cls) {
	case VIRTIO_NET_CTRL_MQ:
		ack = vnet_admin_vf_ctrl_mq_handle(slot, req, cmd);
		break;
	case VIRTIO_NET_CTRL_RX:
		ack = VIRTIO_NET_OK;
		break;
	case VIRTIO_NET_CTRL_MAC:
	case VIRTIO_NET_CTRL_VLAN:
		if (!vnet_admin_vf_ctrl_steering_capture(slot, req, cls, cmd)) {
			DOCA_LOG_WARN("SR-IOV: VF[vf_index=%u] invalid steering ctrl class=%u cmd=%u",
				      slot->vf_index,
				      cls,
				      cmd);
			ack = VIRTIO_NET_ERR;
		}
		break;
	default:
		DOCA_LOG_WARN("SR-IOV: VF[vf_index=%u] unsupported ctrl class %u", slot->vf_index, cls);
		ack = VIRTIO_NET_ERR;
	}

	doca_devemu_vnet_ctrl_req_complete(req, ack, sizeof(uint8_t));
}
