/*
 * Copyright (c) 2025-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <linux/virtio_pci.h>
#include <doca_error.h>
#include <doca_log.h>

#include <doca_devemu_pci_tlp.h>
#include <doca_devemu_vblk_type.h>
#include <doca_devemu_pci_info.h>

#include "vblk_pci_lu.h"
#include "pci_spec_tlp_lu.h"

DOCA_LOG_REGISTER(VBLK_PCI);

static struct doca_devemu_pci_type *vblk_pci_virtio_dev_type;
struct doca_devemu_pci_tlp_channel *vblk_pci_ev_channel;

/**
 * The structs below represent basic PCI bus model
 *
 * - The endpoint type is limited to the virtio block pci device.
 * - Multiple devices are kept in the vblk_pci_space array. The array
 *   is addressable by BDF.
 * - Hot unplug and PCI resets as defined the SPEC 6.6 PCI Express Reset - Rules
 *   are not supported
 * - Initial enumeration and subsequent changing of bus number is supported
 *
 * There is only one pci uplink. It means that in order to have
 * many devices we must implement pci switch.
 *
 * For now we use one g_vblk_pci_virtio_dev endpoint
 */
struct vblk_pci_virtio_dev *g_vblk_pci_dev;
static struct vblk_pci_virtio_dev *vblk_pci_space[256][32][8];

/* VirtIO PCIe device configuration (initialized at runtime by vblk_pci_set_default_values) */
static struct pcie_virtio_dev virtio_dev;

/************************************************************************
 ******      Modular VirtIO PCIe Device Configuration Functions     ******
 ************************************************************************/

/**
 * @brief Setup basic PCI configuration header
 *
 * Configures the standard PCI Type 0 configuration header with VirtIO block
 * device values including vendor ID, device class, and capabilities pointer.
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_pci_header(struct pcie_virtio_dev *dev)
{
	struct pci_config_header_type0 *regs = &dev->cfg.regs;

	regs->vendor_id = VBLK_PCI_VIRTIO_VENDOR_ID;
	regs->subsystem_vendor_id = VBLK_PCI_VIRTIO_VENDOR_ID;
	regs->device_id = 0;		    /* Replaced by real device id at init */
	regs->status = PCI_STATUS_CAP_LIST; /* Cap ptr is valid */
	regs->class_code = PCI_CLASS_STORAGE;
	regs->subclass = PCI_SUBCLASS_STORAGE_NVM;
	regs->revision_id = 1;
	/* Skip pcie cap which is currently not fully supported */
	regs->capabilities_ptr = offsetof(struct pcie_virtio_dev, cfg.msix_cap);
	regs->base_address[VBLK_PCI_VIRTIO_BAR_ID] = PCI_BASE_ADDRESS_MEM_PREFETCH | PCI_BASE_ADDRESS_MEM_TYPE_64;
}

/**
 * @brief Setup PCIe Express capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_pcie_capability(struct pcie_virtio_dev *dev)
{
	struct pcie_capability *pcie_cap = &dev->cfg.pcie_cap;

	pcie_cap->cap.cap_id = PCI_CAP_ID_EXP;
	pcie_cap->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.msix_cap);
}

/**
 * @brief Setup MSI-X capability
 *
 * Configures MSI-X capability with table and PBA locations within the BAR.
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_msix_capability(struct pcie_virtio_dev *dev)
{
	struct msix_capability *msix_cap = &dev->cfg.msix_cap;

	msix_cap->cap.cap_id = PCI_CAP_ID_MSIX;
	msix_cap->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.common_cfg);
	msix_cap->msgctl = VBLK_PCI_MAX_NUM_MSIX - 1; /* MSI-X Disabled by default | Table Size (zero based) */
	/* Note: lower bits are masked and not shifted */
	msix_cap->table_offset = VBLK_PCI_VIRTIO_MSIX_TABLE_OFFSET | VBLK_PCI_VIRTIO_BAR_ID;
	msix_cap->pba_offset = VBLK_PCI_VIRTIO_MSIX_PBA_OFFSET | VBLK_PCI_VIRTIO_BAR_ID;
}

/**
 * @brief Setup VirtIO Common Configuration capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_common_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *common_cfg = &dev->cfg.common_cfg;

	common_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	common_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.notify_cfg);
	common_cfg->cap_len = sizeof(struct virtio_pci_capability);
	common_cfg->cfg_type = VIRTIO_PCI_CAP_COMMON_CFG;
	common_cfg->bar = 0;
	common_cfg->offset = 0;
	common_cfg->length = VBLK_PCI_VIRTIO_PCI_CFG_LEN;
}

/**
 * @brief Setup VirtIO Notification capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_notify_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_notify_capability *notify_cfg = &dev->cfg.notify_cfg;

	notify_cfg->base.cap.cap_id = PCI_CAP_ID_VNDR;
	notify_cfg->base.cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.isr_cfg);
	notify_cfg->base.cap_len = sizeof(struct virtio_pci_notify_capability);
	notify_cfg->base.cfg_type = VIRTIO_PCI_CAP_NOTIFY_CFG;
	notify_cfg->base.bar = 0;
	notify_cfg->base.offset = VBLK_PCI_VIRTIO_DB_OFFSET;
	notify_cfg->base.length = VBLK_PCI_VIRTIO_DB_LEN;
	notify_cfg->notify_off_multiplier = VBLK_PCI_VIRTIO_DB_NOTIFY_BY_OFFSET << VBLK_PCI_VIRTIO_DB_STRIDE;
}

/**
 * @brief Setup VirtIO ISR Status capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_isr_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *isr_cfg = &dev->cfg.isr_cfg;

	isr_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	isr_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.device_cfg);
	isr_cfg->cap_len = sizeof(struct virtio_pci_capability);
	isr_cfg->cfg_type = VIRTIO_PCI_CAP_ISR_CFG;
	isr_cfg->bar = 0;
	isr_cfg->offset = VBLK_PCI_VIRTIO_ISR_CFG_OFFSET;
	isr_cfg->length = VBLK_PCI_VIRTIO_ISR_CFG_LEN;
}

/**
 * @brief Setup VirtIO Device Configuration capability
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_device_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *device_cfg = &dev->cfg.device_cfg;

	device_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	device_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.pci_cfg);
	device_cfg->cap_len = sizeof(struct virtio_pci_capability);
	device_cfg->cfg_type = VIRTIO_PCI_CAP_DEVICE_CFG;
	device_cfg->bar = 0;
	device_cfg->offset = VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;
	device_cfg->length = VBLK_PCI_VIRTIO_DEV_CFG_LEN;
}

/**
 * @brief Setup VirtIO PCI Configuration Access capability (end of chain)
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_virtio_pci_cfg_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_cfg_capability *pci_cfg = &dev->cfg.pci_cfg;

	/* Must be present, but not fully supported.
	 * Spec 4.1.4.9: cap.bar, cap.length, cap.offset and pci_cfg_data
	 * are read-write (RW) for the driver. */
	pci_cfg->base.cap.cap_id = PCI_CAP_ID_VNDR;
	pci_cfg->base.cap.next_ptr = 0; /* End of capability chain */
	pci_cfg->base.cap_len = sizeof(struct virtio_pci_cfg_capability);
	pci_cfg->base.cfg_type = VIRTIO_PCI_CAP_PCI_CFG;
	pci_cfg->base.bar = 0;
	pci_cfg->base.offset = 0;
	pci_cfg->base.length = 0;
}

/**
 * @brief Setup BAR memory mapping configuration
 *
 * @param[out] dev VirtIO device configuration to set up
 */
static void vblk_pci_setup_bar_mapping(struct pcie_virtio_dev *dev)
{
	dev->bar64_map[VBLK_PCI_VIRTIO_BAR_ID].log_size = VBLK_PCI_VIRTIO_BAR_LOG_SIZE;
}

/**
 * @brief Set default values for VirtIO block device configuration
 *
 * Creates a complete VirtIO device configuration with all required capabilities
 * configured according to VirtIO specification. The configuration provides default
 * values that may be overridden by vblk_pci_fixup_virtio_dev() at runtime.
 *
 * @return Complete VirtIO device configuration ready for use
 */
static struct pcie_virtio_dev vblk_pci_set_default_values(void)
{
	struct pcie_virtio_dev dev = {0};

	vblk_pci_setup_pci_header(&dev);
	vblk_pci_setup_pcie_capability(&dev);
	vblk_pci_setup_msix_capability(&dev);
	vblk_pci_setup_virtio_common_capability(&dev);
	vblk_pci_setup_virtio_notify_capability(&dev);
	vblk_pci_setup_virtio_isr_capability(&dev);
	vblk_pci_setup_virtio_device_capability(&dev);
	vblk_pci_setup_virtio_pci_cfg_capability(&dev);
	vblk_pci_setup_bar_mapping(&dev);

	return dev;
}

static void vblk_pci_virtio_dev_queue_init(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	int i;

	pci_cfg->queue_select = 0;

	memset(dev->vqs, 0, sizeof(dev->vqs));
	for (i = 0; i < pci_cfg->num_queues; i++) {
		dev->vqs[i].queue_size = 256;
		/* RO for driver: queue_notify_off; provide a unique notify offset per queue (VirtIO PCI notify cap) */
		dev->vqs[i].queue_notify_off = (uint16_t)i;
		/* Default: no MSI-X vector assigned */
		dev->vqs[i].queue_msix_vector = 0xFFFF;
		/* Disabled by default */
		dev->vqs[i].queue_enable = 0;
	}

	/* Default: no MSI-X vector assigned for config interrupts */
	pci_cfg->config_msix_vector = VIRTIO_MSI_NO_VECTOR;

	memcpy(&pci_cfg->vq, dev->vqs, sizeof(pci_cfg->vq));
}

void vblk_pci_virtio_dev_reset(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;

	dev->driver_features = pci_cfg->driver_feature = 0;
	pci_cfg->driver_feature_select = pci_cfg->device_feature_select = 0;
	/* Keep device_feature aligned with device_feature_select (reset sets select=0 => low 32b features) */
	pci_cfg->device_feature = dev->device_features & 0xFFFFFFFF;

	vblk_pci_virtio_dev_queue_init(dev);
}

void vblk_pci_notify_host(void)
{
	doca_error_t err;

	if (g_vblk_pci_dev->msix && g_vblk_pci_dev->config_msix_vector != g_vblk_pci_dev->pci_cfg.config_msix_vector) {
		if (DOCA_SUCCESS != doca_devemu_pci_msix_destroy(g_vblk_pci_dev->msix)) {
			DOCA_LOG_ERR("Failed to destroy msix: %p", g_vblk_pci_dev->msix);
			return;
		}
		g_vblk_pci_dev->msix = NULL;
	}
	if (!g_vblk_pci_dev->msix && g_vblk_pci_dev->pci_cfg.config_msix_vector != VIRTIO_MSI_NO_VECTOR) {
		err = doca_devemu_pci_ep_create_msix(doca_devemu_pci_tlp_dev_as_ep(g_vblk_pci_dev->pci_tlp_dev),
						     VBLK_PCI_VIRTIO_BAR_ID,
						     VBLK_PCI_VIRTIO_MSIX_TABLE_OFFSET,
						     g_vblk_pci_dev->pci_cfg.config_msix_vector,
						     &g_vblk_pci_dev->msix);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to create the EP msix %d : %s",
				     g_vblk_pci_dev->pci_cfg.config_msix_vector,
				     doca_error_get_descr(err));
			return;
		}
		g_vblk_pci_dev->config_msix_vector = g_vblk_pci_dev->pci_cfg.config_msix_vector;
	}
	if (g_vblk_pci_dev->msix) {
		doca_devemu_pci_msix_raise(g_vblk_pci_dev->msix);
	} else {
		DOCA_LOG_INFO("Skip triggering host MSIX interrupt with vector %#x",
			      g_vblk_pci_dev->pci_cfg.config_msix_vector);
	}
}

static doca_error_t vblk_pci_virtio_dev_init(struct vblk_pci_virtio_dev *dev, const struct vblk_pci_virtio_attrs *attr)
{
	doca_error_t err;
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	const uint16_t num_queues = attr->num_queues;

	/* Only virtio block device is implemented */
	if (attr->device_type != VBLK_PCI_DEVICE_TYPE_VBLK) {
		DOCA_LOG_ERR("unsupported virtio device type %d", attr->device_type);
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	/* Validate num_queues before using it for resource allocation */
	if (num_queues > VBLK_PCI_VIRTIO_MAX_QUEUES) {
		DOCA_LOG_ERR("Invalid number of queues %d > %d", num_queues, VBLK_PCI_VIRTIO_MAX_QUEUES);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* num_msix = min(num_queues + 1, max) - allows VQs to share MSIX when queues > max */
	const uint16_t num_msix = (num_queues + 1 > VBLK_PCI_VIRTIO_MAX_NUM_MSIX) ? VBLK_PCI_VIRTIO_MAX_NUM_MSIX :
										    num_queues + 1;

	err = doca_devemu_pci_ep_set_num_msix(doca_devemu_pci_tlp_dev_as_ep(dev->pci_tlp_dev), num_msix);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set num_msix=%u: %s", num_msix, doca_error_get_descr(err));
		return err;
	}

	err = doca_devemu_pci_ep_set_num_db(doca_devemu_pci_tlp_dev_as_ep(dev->pci_tlp_dev), num_queues);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set num_db=%u: %s", num_queues, doca_error_get_descr(err));
		return err;
	}
	DOCA_LOG_INFO("Endpoint: num_msix=%u, num_db=%u", num_msix, num_queues);

	dev->pcie_dev = virtio_dev;
	dev->pcie_dev.cfg.msix_cap.msgctl = num_msix - 1; /* zero based */
	DOCA_LOG_INFO("Endpoint msgctl=%u (table_size=%u)", dev->pcie_dev.cfg.msix_cap.msgctl, num_msix);

	/* 4.1.2.1 Device Requirements: PCI Device Discovery
	 * Devices MUST either have the PCI Device ID calculated by adding 0x1040 to the Virtio Device ID
	 */
	dev->pcie_dev.cfg.regs.device_id = VBLK_PCI_VIRTIO_NON_TRANSITION_DEV_ID_BASE + attr->device_type;
	dev->pcie_dev.cfg.regs.subsystem_id = attr->device_type;

	pci_cfg->num_queues = attr->num_queues;

	dev->device_features = attr->device_features | (1ULL << VBLK_PCI_VIRTIO_F_VERSION_1) |
			       (1ULL << VBLK_PCI_VIRTIO_F_ACCESS_PLATFORM);

	pci_cfg->device_feature = dev->device_features & 0xFFFFFFFF;
	pci_cfg->device_status = VBLK_PCI_VIRTIO_DEVICE_STATUS_NEEDS_RESET;

	if (attr->dev_cfg) {
		memcpy(&dev->vblk_cfg, attr->dev_cfg, sizeof(dev->vblk_cfg));
		if (dev->vblk_cfg.num_queues > pci_cfg->num_queues) {
			DOCA_LOG_ERR("Invalid number of the device queues: dev_cfg %d > pci_cfg %d",
				     dev->vblk_cfg.num_queues,
				     pci_cfg->num_queues);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	vblk_pci_virtio_dev_queue_init(dev);

	dev->pci_cfg_change_cb = attr->pci_cfg_change_cb;
	dev->cb_arg = attr->cb_arg;
	return DOCA_SUCCESS;
}

/**
 * @brief Fixup VirtIO device configuration with runtime PCI type info
 *
 * Queries the DOCA PCI type for actual BAR layout, region offsets, and MSIX/PBA
 * locations, then updates the configuration accordingly. Must be called after
 * doca_devemu_pci_type_start().
 *
 * @param[in] virtio_type DOCA PCI type handle
 * @param[in,out] dev VirtIO device configuration to fixup
 * @return DOCA_SUCCESS on success, DOCA_ERROR_* on failure
 */
static doca_error_t vblk_pci_fixup_virtio_dev(struct doca_devemu_pci_type *virtio_type, struct pcie_virtio_dev *dev)
{
	doca_error_t err, ret = DOCA_SUCCESS;
	uint32_t i, num_bars, num_regions;
	struct doca_devemu_pci_bar_info **bar_list;
	struct doca_devemu_pci_transaction_region_info **mmio_list;
	struct doca_devemu_pci_db_region_by_data_info **db_list;
	struct doca_devemu_pci_msix_pba_region_info **pba_list;
	struct doca_devemu_pci_msix_table_region_info **msix_list;

	/* to improve debugability it is better to do full pci type scan and
	 * not to stop on non fatal errors
	 */
	err = doca_devemu_pci_type_create_bar_info_list(virtio_type, &bar_list, &num_bars);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio bar list");
		return err;
	}

	for (i = 0; i < num_bars; i++) {
		uint8_t id, log_sz, is_prefetch;
		enum doca_devemu_pci_bar_mem_type mem_type;
		uint32_t val = 0;

		doca_devemu_pci_bar_info_get_bar_id(bar_list[i], &id);
		doca_devemu_pci_bar_info_get_log_sz(bar_list[i], &log_sz);
		doca_devemu_pci_bar_info_get_mem_type(bar_list[i], &mem_type);
		doca_devemu_pci_bar_info_get_prefetchable(bar_list[i], &is_prefetch);

		DOCA_LOG_INFO("bar%d log %d mem_type %d prefetch %d", id, log_sz, mem_type, is_prefetch);
		if (is_prefetch)
			val |= PCI_BASE_ADDRESS_MEM_PREFETCH;
		if (mem_type == DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT) {
			val |= PCI_BASE_ADDRESS_MEM_TYPE_64;
			dev->bar64_map[id].log_size = log_sz;
			dev->cfg.regs.base_address[id] = val;
		} else if (mem_type == DOCA_DEVEMU_PCI_BAR_MEM_TYPE_1_MB) {
			DOCA_LOG_ERR("bar memtype 1_MB is not supported");
			goto free_bar_list;
		} else {
			if (log_sz) {
				DOCA_LOG_ERR("32bit bars are not supported");
				goto free_bar_list;
			}
		}
	}
	doca_devemu_pci_type_destroy_bar_info_list(bar_list);

	err = doca_devemu_pci_type_create_transaction_region_info_list(virtio_type, &mmio_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio MMIO TLP region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one MMIO TLP region");
		ret = DOCA_ERROR_NOT_SUPPORTED;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one MMIO TLP region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_transaction_region_info_get_bar_id(mmio_list[i], &id);
		doca_devemu_pci_transaction_region_info_get_start_addr(mmio_list[i], &start_addr);
		doca_devemu_pci_transaction_region_info_get_size(mmio_list[i], &size);

		DOCA_LOG_INFO("mmio_tlp:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		/* region size must be big enough for our desired virtio bar config */
		if (size < VBLK_PCI_VIRTIO_DEV_CFG_OFFSET + VBLK_PCI_VIRTIO_DEV_CFG_LEN) {
			DOCA_LOG_ERR("region size is too small");
			ret = DOCA_ERROR_INVALID_VALUE;
		}

		dev->cfg.common_cfg.bar = id;
		dev->cfg.common_cfg.offset = start_addr;

		dev->cfg.isr_cfg.bar = id;
		dev->cfg.isr_cfg.offset = start_addr + VBLK_PCI_VIRTIO_ISR_CFG_OFFSET;

		dev->cfg.device_cfg.bar = id;
		dev->cfg.device_cfg.offset = start_addr + VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;

		dev->cfg.pci_cfg.base.bar = id;
	}

	doca_devemu_pci_type_destroy_transaction_region_info_list(mmio_list);

	err = doca_devemu_pci_type_create_db_region_by_data_info_list(virtio_type, &db_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio db region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one doorbell region");
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one doorbell region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_db_region_by_data_info_get_bar_id(db_list[i], &id);
		doca_devemu_pci_db_region_by_data_info_get_start_addr(db_list[i], &start_addr);
		doca_devemu_pci_db_region_by_data_info_get_size(db_list[i], &size);

		DOCA_LOG_INFO("db_region:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		/* Assume stride and doorbell size are standard */
		dev->cfg.notify_cfg.base.bar = id;
		dev->cfg.notify_cfg.base.offset = start_addr;
		dev->cfg.notify_cfg.base.length = size;
	}

	doca_devemu_pci_type_destroy_db_region_by_data_info_list(db_list);

	err = doca_devemu_pci_type_create_msix_table_region_info_list(virtio_type, &msix_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio MSIX region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one MSIX region");
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one MSIX region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_msix_table_region_info_get_bar_id(msix_list[i], &id);
		doca_devemu_pci_msix_table_region_info_get_start_addr(msix_list[i], &start_addr);
		doca_devemu_pci_msix_table_region_info_get_size(msix_list[i], &size);

		DOCA_LOG_INFO("msix_region:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		dev->cfg.msix_cap.table_offset = start_addr | id;
	}

	doca_devemu_pci_type_destroy_msix_table_region_info_list(msix_list);

	err = doca_devemu_pci_type_create_msix_pba_region_info_list(virtio_type, &pba_list, &num_regions);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio PBA region list");
		return err;
	}

	if (num_regions == 0) {
		DOCA_LOG_ERR("virtio type must have one PBA region");
		ret = DOCA_ERROR_INVALID_VALUE;
	}

	if (num_regions > 1)
		DOCA_LOG_WARN("Expecting one PBA region got %d. Using first region", num_regions);

	for (i = 0; i < num_regions; i++) {
		uint8_t id;
		uint64_t start_addr, size;

		doca_devemu_pci_msix_pba_region_info_get_bar_id(pba_list[i], &id);
		doca_devemu_pci_msix_pba_region_info_get_start_addr(pba_list[i], &start_addr);
		doca_devemu_pci_msix_pba_region_info_get_size(pba_list[i], &size);

		DOCA_LOG_INFO("pba_region:%d bar%d start 0x%lx size 0x%lx", i, id, start_addr, size);
		if (i > 0)
			continue;

		dev->cfg.msix_cap.pba_offset = start_addr | id;
	}

	doca_devemu_pci_type_destroy_msix_pba_region_info_list(pba_list);

	return ret;

free_bar_list:
	doca_devemu_pci_type_destroy_bar_info_list(bar_list);
	return DOCA_ERROR_INVALID_VALUE;
}

static doca_error_t vblk_pci_virtio_block_init(struct doca_dev *dev)
{
	doca_error_t err;

	err = doca_devemu_vblk_pci_tlp_type_create("vblk_pci_vblk", &vblk_pci_virtio_dev_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create virtio block tlp device type");
		return err;
	}

	err = doca_devemu_pci_type_set_dev(vblk_pci_virtio_dev_type, dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach device to virtio pci type");
		goto config_failed;
	}

	err = doca_devemu_pci_tlp_type_set_pci_cap_conf(vblk_pci_virtio_dev_type,
							PCI_CAP_ID_EXP,
							offsetof(struct pcie_virtio_dev, cfg.pcie_cap),
							sizeof(struct pcie_capability));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set PCI capability configuration");
		goto config_failed;
	}

	err = doca_devemu_pci_tlp_type_set_pci_cap_conf(vblk_pci_virtio_dev_type,
							PCI_CAP_ID_MSIX,
							offsetof(struct pcie_virtio_dev, cfg.msix_cap),
							sizeof(struct msix_capability));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set MSI-X capability configuration: %s", doca_error_get_descr(err));
		goto config_failed;
	}

	err = doca_devemu_pci_type_set_num_msix(vblk_pci_virtio_dev_type, VBLK_PCI_VIRTIO_MAX_NUM_MSIX);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set number of MSIX interrupts");
		goto config_failed;
	}

	err = doca_devemu_pci_type_set_num_db(vblk_pci_virtio_dev_type, VBLK_PCI_VIRTIO_MAX_NUM_DB);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set number of doorbell registers");
		goto config_failed;
	}
	DOCA_LOG_INFO("Type: num_msix=%u, num_db=%u", VBLK_PCI_VIRTIO_MAX_NUM_MSIX, VBLK_PCI_VIRTIO_MAX_NUM_DB);

	err = doca_devemu_pci_type_start(vblk_pci_virtio_dev_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start virtio block pci type");
		goto config_failed;
	}

	DOCA_LOG_INFO("Created Virtio Block pci device type");

	/* Create default device configuration and fixup with runtime PCI type info */
	virtio_dev = vblk_pci_set_default_values();
	err = vblk_pci_fixup_virtio_dev(vblk_pci_virtio_dev_type, &virtio_dev);
	if (err != DOCA_SUCCESS)
		goto fixup_failed;

	return DOCA_SUCCESS;

fixup_failed:
	if (DOCA_SUCCESS != doca_devemu_pci_type_stop(vblk_pci_virtio_dev_type)) {
		DOCA_LOG_ERR("Failed to stop vblk_pci_virtio_dev_type: %p", vblk_pci_virtio_dev_type);
	}
config_failed:
	if (DOCA_SUCCESS != doca_devemu_pci_type_destroy(vblk_pci_virtio_dev_type)) {
		DOCA_LOG_ERR("Failed to destroy vblk_pci_virtio_dev_type: %p", vblk_pci_virtio_dev_type);
	}
	return err;
}

static void vblk_pci_virtio_reset(void)
{
	if (DOCA_SUCCESS != doca_devemu_pci_type_stop(vblk_pci_virtio_dev_type)) {
		DOCA_LOG_ERR("Failed to stop vblk_pci_virtio_dev_type: %p", vblk_pci_virtio_dev_type);
	}

	if (DOCA_SUCCESS != doca_devemu_pci_type_destroy(vblk_pci_virtio_dev_type)) {
		DOCA_LOG_ERR("Failed to destroy vblk_pci_virtio_dev_type: %p", vblk_pci_virtio_dev_type);
	}
}

static inline uint64_t vblk_pci_bar_offset(struct vblk_pci_virtio_dev *dev, const uint64_t addr)
{
	return addr - (dev->pcie_dev.cfg.regs.base_address64[VBLK_PCI_VIRTIO_BAR_ID] & ~0xF);
}

static uint32_t vblk_pci_virtio_mmio_read32(struct vblk_pci_virtio_dev *dev, const uint64_t addr)
{
	uint64_t offset = vblk_pci_bar_offset(dev, addr);
	uint32_t *base;

	DOCA_LOG_DBG("mmio read offset 0x%lx", offset);

	/* cast is needed to avoid always true comparison warning when defined PCI_CFG_OFFSET is 0 */
	if ((int)offset >= (int)VBLK_PCI_VIRTIO_PCI_CFG_OFFSET &&
	    offset < VBLK_PCI_VIRTIO_PCI_CFG_OFFSET + sizeof(dev->pci_cfg)) {
		offset -= VBLK_PCI_VIRTIO_PCI_CFG_OFFSET;
		base = (uint32_t *)&dev->pci_cfg;
	} else if (offset >= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET &&
		   offset < VBLK_PCI_VIRTIO_DEV_CFG_OFFSET + sizeof(dev->dev_cfg)) {
		offset -= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;
		base = (uint32_t *)&dev->dev_cfg;
	} else {
		DOCA_LOG_ERR("mmio read - unsupported virtio bar offset 0x%lx", offset);
		return 0xFAFAFAFA;
	}
	return base[offset / 4];
}

static inline uint32_t vblk_pci_feature_select(int n, uint64_t ftr)
{
	if (n > 1)
		return 0;

	return n ? ftr >> 32 : ftr & 0xFFFFFFFF;
}

/* rw pointer to the current pci configuration */
struct vblk_pci_virtio_pci_common_cfg *vblk_pci_virtio_get_pci_cfg(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	uint32_t *p = (uint32_t *)&dev->driver_features;

	/* sync features and queue values */

	if (pci_cfg->driver_feature_select <= 1)
		p[pci_cfg->driver_feature_select] = pci_cfg->driver_feature;

	return pci_cfg;
}

/* ro queues */
const struct vblk_pci_virtq_pci_cfg *vblk_pci_virtio_get_virtq_pci_cfg(struct vblk_pci_virtio_dev *dev)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;

	/* sync queues */
	if (pci_cfg->queue_select < pci_cfg->num_queues && pci_cfg->queue_select < VBLK_PCI_VIRTIO_MAX_QUEUES) {
		/* coverity[OVERRUN] */
		memcpy(&dev->vqs[pci_cfg->queue_select], &pci_cfg->vq, sizeof(pci_cfg->vq));
	}

	return dev->vqs;
}

/* rw device config */
const struct vblk_pci_virtio_blk_config *vblk_pci_virtio_get_vblk_dev_cfg(struct vblk_pci_virtio_dev *dev)
{
	return &dev->vblk_cfg;
}

void vblk_pci_set_capacity(uint64_t capacity_bytes)
{
	struct vblk_pci_virtio_dev *dev = g_vblk_pci_dev;

	if (dev == NULL) {
		DOCA_LOG_ERR("Cannot set capacity: device not initialized");
		return;
	}
	dev->vblk_cfg.capacity = capacity_bytes / 512;
	DOCA_LOG_INFO("Block device capacity updated: %lu bytes (%lu sectors)", capacity_bytes, dev->vblk_cfg.capacity);
}

static void vblk_pci_virtio_pci_cfg_write32(struct vblk_pci_virtio_dev *dev,
					    const uint64_t offset,
					    const uint32_t val,
					    const uint32_t wr_mask)
{
	struct vblk_pci_virtio_pci_common_cfg *pci_cfg = &dev->pci_cfg;
	struct vblk_pci_virtio_pci_common_cfg prev_cfg;
	uint32_t *base = (uint32_t *)pci_cfg;

	/* save everything to handle ro fields and selectors */
	memcpy(&prev_cfg, pci_cfg, sizeof(prev_cfg));

	const uint32_t dw_offset = offset / 4;
	const uint32_t orig_val = base[dw_offset];

	base[dw_offset] = (orig_val & ~wr_mask) | (val & wr_mask);
	DOCA_LOG_INFO("pci_cfg+%ld: 0x%x <- 0x%x (val 0x%x & mask 0x%x)",
		      offset,
		      orig_val,
		      base[dw_offset],
		      val,
		      wr_mask);

	/* check RO fields */
	if (prev_cfg.device_feature != pci_cfg->device_feature || prev_cfg.num_queues != pci_cfg->num_queues ||
	    prev_cfg.config_generation != pci_cfg->config_generation ||
	    prev_cfg.admin_queue_index != pci_cfg->admin_queue_index ||
	    prev_cfg.admin_queue_num != pci_cfg->admin_queue_num ||
	    prev_cfg.vq.queue_notify_off != pci_cfg->vq.queue_notify_off ||
	    prev_cfg.vq.queue_notif_config_data != pci_cfg->vq.queue_notif_config_data) {
		DOCA_LOG_ERR("pci_cfg: attempt to overwrite RO field: ROLLBACK");
		goto rollback;
	}

	if (prev_cfg.device_feature_select != pci_cfg->device_feature_select) {
		DOCA_LOG_INFO("dev_ftr select: %d -> %d",
			      prev_cfg.device_feature_select,
			      pci_cfg->device_feature_select);
		pci_cfg->device_feature = vblk_pci_feature_select(pci_cfg->device_feature_select, dev->device_features);
	}

	if (prev_cfg.driver_feature_select != pci_cfg->driver_feature_select) {
		DOCA_LOG_INFO("drv_ftr select: %d -> %d",
			      prev_cfg.driver_feature_select,
			      pci_cfg->driver_feature_select);
		uint32_t *p = (uint32_t *)&dev->driver_features;
		if (prev_cfg.driver_feature_select <= 1)
			p[prev_cfg.driver_feature_select] = pci_cfg->driver_feature;

		pci_cfg->driver_feature = vblk_pci_feature_select(pci_cfg->driver_feature_select, dev->driver_features);
	}

	if (prev_cfg.queue_select != pci_cfg->queue_select) {
		DOCA_LOG_INFO("queue select: %d -> %d", prev_cfg.queue_select, pci_cfg->queue_select);

		if (prev_cfg.queue_select < pci_cfg->num_queues && prev_cfg.queue_select < VBLK_PCI_VIRTIO_MAX_QUEUES) {
			/* coverity[OVERRUN] */
			memcpy(&dev->vqs[prev_cfg.queue_select], &pci_cfg->vq, sizeof(pci_cfg->vq));
		}

		/* 4.1.4.3.1 Device Requirements: Common configuration structure layout
		 * The device MUST present a 0 in queue_size if the virtqueue corresponding to the current queue_select
		 * is unavailable.
		 */
		if (pci_cfg->queue_select < prev_cfg.num_queues && pci_cfg->queue_select < VBLK_PCI_VIRTIO_MAX_QUEUES) {
			/* coverity[OVERRUN] */
			memcpy(&pci_cfg->vq, &dev->vqs[pci_cfg->queue_select], sizeof(pci_cfg->vq));
		} else {
			memset(&pci_cfg->vq, 0, sizeof(pci_cfg->vq));
		}
	}

	/* Helpful debug: log when the driver enables/disables the currently selected queue */
	if (prev_cfg.queue_select == pci_cfg->queue_select && prev_cfg.vq.queue_enable != pci_cfg->vq.queue_enable) {
		DOCA_LOG_INFO("queue_enable qid=%u %u -> %u",
			      pci_cfg->queue_select,
			      prev_cfg.vq.queue_enable,
			      pci_cfg->vq.queue_enable);
	}

	if (dev->pci_cfg_change_cb)
		dev->pci_cfg_change_cb(dev, dev->cb_arg);

	if (prev_cfg.device_status != pci_cfg->device_status) {
		DOCA_LOG_INFO("device status: %d -> %d", prev_cfg.device_status, pci_cfg->device_status);
		if (pci_cfg->device_status == 0)
			vblk_pci_virtio_dev_reset(dev);
	}

	return;

rollback:
	memcpy(pci_cfg, &prev_cfg, sizeof(prev_cfg));
}

static void vblk_pci_virtio_dev_cfg_write32(struct vblk_pci_virtio_dev *virtio_dev,
					    const uint64_t offset,
					    const uint32_t val,
					    const uint32_t wr_mask)
{
	uint32_t *base = (uint32_t *)&virtio_dev->dev_cfg;
	const uint32_t orig_val = base[offset / 4];

	/* RO for the virtio block with the exception of writeback field when
	 * VIRTIO_BLK_F_CONFIG_WCE is negotiated.
	 * SPEC 5.2.5 Device Initialization
	 */
	DOCA_LOG_WARN("write to the READ-ONLY field dev_cfg+%ld: 0x%x <- 0x%x (val 0x%x & mask 0x%x)",
		      offset,
		      orig_val,
		      (orig_val & ~wr_mask) | (val & wr_mask),
		      val,
		      wr_mask);
}

static void vblk_pci_virtio_mmio_write32(struct vblk_pci_virtio_dev *dev,
					 const uint64_t addr,
					 const uint32_t val,
					 const uint32_t wr_mask)
{
	/* implement actual mmio write */
	uint64_t offset = vblk_pci_bar_offset(dev, addr);

	DOCA_LOG_DBG("mmio write offset 0x%lx", offset);

	/* cast is needed to avoid always true comparison warning when defined PCI_CFG_OFFSET is 0 */
	if ((int)offset >= (int)VBLK_PCI_VIRTIO_PCI_CFG_OFFSET &&
	    offset < VBLK_PCI_VIRTIO_PCI_CFG_OFFSET + sizeof(dev->pci_cfg)) {
		offset -= VBLK_PCI_VIRTIO_PCI_CFG_OFFSET;
		vblk_pci_virtio_pci_cfg_write32(dev, offset, val, wr_mask);
	} else if (offset >= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET &&
		   offset < VBLK_PCI_VIRTIO_DEV_CFG_OFFSET + sizeof(dev->dev_cfg)) {
		offset -= VBLK_PCI_VIRTIO_DEV_CFG_OFFSET;
		vblk_pci_virtio_dev_cfg_write32(dev, offset, val, wr_mask);
	} else {
		DOCA_LOG_ERR("mmio write drop - unsupported virtio bar offset 0x%lx", offset);
		return;
	}

	/* consider: raise bar change event so that app can address it */
}

static struct vblk_pci_virtio_dev *vblk_pci_dev_find_by_addr(const uint64_t addr)
{
	struct vblk_pci_virtio_dev *dev = g_vblk_pci_dev;

	/* in generally we have to match all bars, but for the virtio we only need one */
	const int log_size = dev->pcie_dev.bar64_map[VBLK_PCI_VIRTIO_BAR_ID].log_size;
	/* clear type bits */
	const uint64_t bar_base_addr = dev->pcie_dev.cfg.regs.base_address64[VBLK_PCI_VIRTIO_BAR_ID] & ~0xF;

	if (addr >= bar_base_addr && addr < bar_base_addr + (1 << log_size))
		return dev;

	return NULL;
}

static struct vblk_pci_virtio_dev *vblk_pci_dev_find(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);

	uint8_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint8_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint8_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);

	return vblk_pci_space[bus][device][function];
}

static struct vblk_pci_virtio_dev *vblk_pci_dev_add(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);

	uint8_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint8_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint8_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);
	struct vblk_pci_virtio_dev *dev = g_vblk_pci_dev;

	if (!dev) {
		DOCA_LOG_ERR("there are no free devices - unable to handle enumeration request");
		return NULL;
	}

	/* enumeration can change bus/device but not function */
	if (function != dev->function) {
		DOCA_LOG_DBG("ignoring bdf %x:%x.%x", bus, device, function);
		return NULL;
	}

	if (!dev->is_enumerated)
		dev->is_enumerated = true;
	else
		vblk_pci_space[dev->bus][dev->device][dev->function] = NULL;

	dev->bus = bus;
	dev->device = device;
	dev->function = function;

	vblk_pci_space[bus][device][function] = dev;
	DOCA_LOG_INFO("assigned bdf %02x:%02x.%x", bus, device, function);
	return dev;
}

static void vblk_pci_tlp_cpl_prep_dw0(struct doca_devemu_pci_tlp_channel_req *tlp_req, int cmpl_fmt, int cmpl_length)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	/* DW0: fmt[31:29], type[28:24], length[9:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), cmpl_fmt, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), TLP_TYPE_COMPLETION, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(23, 23), GET_TLP_REQ_TAG9(tlp_req_header), &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(19, 19), GET_TLP_REQ_TAG8(tlp_req_header), &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), cmpl_length, &header_dw[0]);
}

static void vblk_pci_tlp_cpl_prep_dw1_2(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					const uint32_t completer_id,
					const int cmpl_status,
					const int byte_count)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	/* DW1: completer_id[31:16], cmpl_status[15:13], byte_cnt[11:0] (set to 0x04) */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), completer_id, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 13), cmpl_status, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(11, 0), byte_count, &header_dw[1]);

	/* DW2: requester_id[31:16], tag[15:8] (set to 0), lower_addr[6:0] (set to 0) */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), GET_TLP_REQ_REQ_ID(tlp_req_header), &header_dw[2]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), GET_TLP_REQ_TAG(tlp_req_header), &header_dw[2]);
}

static void vblk_pci_tlp_cfg_cpl_prep(struct doca_devemu_pci_tlp_channel_req *tlp_req,
				      int cmpl_fmt,
				      int cmpl_status,
				      int cmpl_length)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	memset(header_dw, 0, 3 * sizeof(uint32_t));

	vblk_pci_tlp_cpl_prep_dw0(tlp_req, cmpl_fmt, cmpl_length);

	/* cfg requests are always bdf routed */
	uint16_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint16_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint16_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);
	uint16_t completer_id = (bus << 8) | (device << 3) | function;

	/* SPEC:
	 * - For Memory Read Completions, Byte Count[11:0] is set according to the rules in § Section
	 * 2.3.1.1
	 * - For AtomicOp Completions, the Byte Count value must equal the associated AtomicOp
	 *   operand size in bytes
	 * - For all other types of Completions, the Byte Count value must be 4
	 */
	vblk_pci_tlp_cpl_prep_dw1_2(tlp_req, completer_id, cmpl_status, 4);
}

static void vblk_pci_tlp_mmio_cpl_prep(struct vblk_pci_virtio_dev *virtio_dev,
				       struct doca_devemu_pci_tlp_channel_req *tlp_req,
				       int cmpl_fmt,
				       int cmpl_status,
				       int cmpl_length,
				       int byte_count)
{
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	memset(header_dw, 0, 3 * sizeof(uint32_t));
	vblk_pci_tlp_cpl_prep_dw0(tlp_req, cmpl_fmt, cmpl_length);

	const uint16_t completer_id = (virtio_dev->bus << 8) | (virtio_dev->device << 3) | virtio_dev->function;

	vblk_pci_tlp_cpl_prep_dw1_2(tlp_req, completer_id, cmpl_status, byte_count);
}

static uint32_t vblk_pci_bit_enable_to_mask(const uint32_t be_mask)
{
	uint32_t bit_mask = 0;

	if (be_mask & 0x1)
		bit_mask = 0xFFu;

	if (be_mask & (0x1 << 1))
		bit_mask |= (0xFFu << 8);

	if (be_mask & (0x1 << 2))
		bit_mask |= (0xFFu << 16);

	if (be_mask & (0x1 << 3))
		bit_mask |= (0xFFu << 24);

	return bit_mask;
}

static uint32_t vblk_pci_ext_reg_wr_mask(struct vblk_pci_virtio_dev *virtio_dev, const uint32_t ext_reg_num)
{
	struct pcie_raw_cfg *pcie_dev = TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev);

	switch (ext_reg_num) {
	/* Registers that can be treated as RO */
	case 0: /* vendor_id, device id - HwInit */
	case 2: /* revision_id HwInit, (prog_if, subclass, class_code) - RO */
	case 3:
		/*
		 * cache_line_size - RO, see comment
		 * SPEC: 7.5.1.1.7 Cache Line Size Register (Offset 0Ch)
		 * This read-write register is implemented for legacy compatibility purposes but has no
		 * effect on any PCI Express device behavior
		 *
		 * latency_timer - RO, see comment
		 * SPEC: 7.5.1.1.8 Latency Timer Register (Offset 0Dh)
		 * Its functionality does not apply to PCI Express.
		 * This register must be hardwired to 00h.
		 *
		 * header_type - RO
		 * bist - RO (not capable)
		 */
	case 10: /* cardbus_cis
		  * SPEC: 7.5.1.2.2 Cardbus CIS Pointer Register (Offset 28h)
		  * This register does not apply to PCI Express and must be hardwired
		  * to Zero
		  */
	case 11: /* subsystem_vendor_id, subsystem_id - RO */
	case 12: /* rom_address - not supported, RO, set to 0 */
	case 13: /* capabilities_ptr, HwInit */
	case 14: /* reserved */
	case 15: /* interrupt_line - RO, 0,
		  * SPEC: 7.5.1.1.12 Interrupt Line Register (Offset 3Ch)
		  * If Interrupt Pin Register is 00h, this register is
		  * permitted to be hardwired to 0b.
		  *
		  * interrupt_pin - RO 0
		  * SPEC: 7.5.1.1.13 Interrupt Pin Register (Offset 3Dh)
		  * A value of 00h indicates that the Function uses no legacy interrupt Message(s)
		  *
		  * min_gnt, max_lat - RO 0
		  * SPEC: 7.5.1.2.5 Min_Gnt Register/Max_Lat Register (Offset 3Eh/3Fh)
		  * These registers do not apply to PCI Express and must be hardwired to Zero.
		  */
		return 0;
	}

	/* BARs
	 * note that we only support 64 bit bars
	 */
	if (ext_reg_num >= 4 && ext_reg_num < 10) {
		const int bar64_id = (ext_reg_num - 4) / 2;

		if (pcie_dev->bar64_map[bar64_id].log_size == 0)
			return 0;

		if ((ext_reg_num - 4) % 2)
			return 0xFFFFFFFF;

		/* SPEC: Base Address Registers (Offset 10h - 24h)
		 * Bits 3-0 are  read-only.
		 */
		return ~((1 << pcie_dev->bar64_map[bar64_id].log_size) - 1) & (~0xF);
	}

	/* Status & command registers */
	if (ext_reg_num == 1) {
		/* Command reg RO and bits that can be assumed RO:
		 * SPEC: 7.5.1.1.3 Command Register (Offset 04h)
		 * 0 - PCI_COMMAND_IO
		 * This bit is permitted to be hardwired to Zero if a Function does not support I/O Space accesses
		 *
		 * 3 - PCI_COMMAND_SPECIAL
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 4 - PCI_COMMAND_INVALIDATE
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 5 - PCI_COMMAND_VGA_PALETTE
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 6 - PCI_COMMAND_PARITY
		 * An RCiEP that is not associated with a Root Complex Event Collector is permitted to hardwire this bit
		 * to 0b.
		 *
		 * 7 - PCI_COMMAND_WAIT
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 8 - PCI_COMMAND_SERR
		 * An RCiEP that is not associated with a Root Complex Event Collector is permitted to hardwire this bit
		 * to
		 *
		 * 9 - PCI_COMMAND_FAST_BACK
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 10 - PCI_COMMAND_INT_DISABLE
		 * For Functions with a Type 0 Configuration Space Header that do not generate INTx interrupts,
		 * this bit is optional. If not implemented, this bit must be hardwired to 0b
		 *
		 * 11 - 15 - PCI_COMMAND_RES_BIT_MASK
		 */
		const uint16_t command_ro_mask = PCI_COMMAND_IO | PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
						 PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_PARITY | PCI_COMMAND_WAIT |
						 PCI_COMMAND_SERR | PCI_COMMAND_FAST_BACK | PCI_COMMAND_INT_DISABLE |
						 PCI_COMMAND_RES_BIT_MASK;

		/* Status reg RO and bits that can be assumed RO:
		 * 7.5.1.1.4 Status Register (Offset 06h)
		 * 0 - PCI_STATUS_READY
		 * This optional bit, when Set, indicates the Function is guaranteed to be ready to
		 * successfully complete valid Configuration Requests at any time.
		 *
		 * 1 - 2 reserved
		 *
		 * 3 - PCI_STATUS_INTERRUPT
		 * Functions that do not generate INTx interrupts are permitted to hardwire this bit to 0b.
		 *
		 * 4 - PCI_STATUS_CAP_LIST
		 *
		 * 5 - PCI_STATUS_66MHZ
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 6 - reserved
		 *
		 * 7 - PCI_STATUS_FAST_BACK
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 8 - PCI_STATUS_PARITY RW1C but we can treat it as RO because we do not report parity errors
		 * If the Parity Error Response bit is 0b, this bit is never Set.
		 *
		 * 9 - 10 PCI_STATUS_DEVSEL_MASK
		 * Its functionality does not apply to PCI Express and the bit must be hardwired to 0b
		 *
		 * 11 - PCI_STATUS_SIG_TARGET_ABORT RW1C can treat as RO
		 * Functions with a Type 0 Configuration Space Header that do not signal Completer Abort are permitted
		 * to hardwire this bit to 0b.
		 *
		 * 12 - PCI_STATUS_REC_TARGET_ABORT RW1C
		 * RO because functionality is not implemented
		 *
		 * 13 - PCI_STATUS_REC_MASTER_ABORT RW1C
		 * RO because functionality is not implemented
		 *
		 * 14 - PCI_STATUS_SIG_SYSTEM_ERROR RW1C, RO because SERR always 0
		 *
		 * This bit is Set when a Function sends an ERR_FATAL or ERR_NONFATAL Message, and the SERR# Enable
		 * bit in the Command Register is 1b
		 *
		 * Functions with a Type 0 Configuration Space Header that do not send ERR_FATAL or ERR_NONFATAL
		 * Messages are permitted to hardwire this bit to 0b
		 *
		 * 15 - PCI_STATUS_DETECTED_PARITY
		 * RO because functionality is not implemented
		 *
		 */
		/* bottom line we can treat status register as RO */
		const uint16_t status_ro_mask = 0xFFFF;

		return ~(((uint32_t)status_ro_mask << 16) | (uint32_t)command_ro_mask);
	}

	/* everything else is writable */
	return 0xFFFFFFFF;
}

static void vblk_pci_handle_cfg_write0(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	/* currently data follow header automagically */
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t *tlp_req_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);	/* data */
	const uint32_t ext_reg_num = GET_TLP_REQ_EXT_REG_NUM(tlp_req_header);
	struct vblk_pci_virtio_dev *virtio_dev = vblk_pci_dev_find(tlp_req);
	const uint32_t be_mask = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	const uint32_t dw_input = tlp_req_data[0];

	uint32_t dw_mask, dw_read, dw_write;

	DOCA_LOG_DBG("CFG write ext_reg_num %d mask 0x%x input 0x%x", ext_reg_num, be_mask, dw_input);

	if (!virtio_dev) {
		/* must be device enumeration, add to the device space */
		DOCA_LOG_DBG("ENUMERATION CFG write ext_reg_num %d mask 0x%x input 0x%x",
			     ext_reg_num,
			     be_mask,
			     dw_input);
		if (ext_reg_num != 0) {
			DOCA_LOG_ERR("expected enumeration request (ext reg = 0)");
			DOCA_LOG_ERR("CFG write ext_reg_num %d mask 0x%x input 0x%x", ext_reg_num, be_mask, dw_input);
			goto fatal_error;
		}

		virtio_dev = vblk_pci_dev_add(tlp_req);
		if (!virtio_dev)
			goto fatal_error;
	}

	dw_mask = vblk_pci_bit_enable_to_mask(be_mask) & vblk_pci_ext_reg_wr_mask(virtio_dev, ext_reg_num);
	dw_read = pcie_config_read(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), ext_reg_num);
	dw_write = (~dw_mask & dw_read) | (dw_input & dw_mask);

	DOCA_LOG_DBG("CFG write result: input 0x%x read 0x%x mask 0x%x -> 0x%x", dw_input, dw_read, dw_mask, dw_write);

	pcie_config_write(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), ext_reg_num, dw_write);

	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0);

	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
	return;

fatal_error:
	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
}

static void vblk_pci_handle_cfg_read0(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t ext_reg_num = GET_TLP_REQ_EXT_REG_NUM(tlp_req_header);
	const uint32_t be_mask = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	struct vblk_pci_virtio_dev *virtio_dev = vblk_pci_dev_find(tlp_req);

	uint32_t dw_mask, dw_reg, completion_data;
	void *tlp_cpl_data;

	DOCA_LOG_DBG("CFG read ext_reg_num %d mask 0x%x", ext_reg_num, be_mask);

	if (!virtio_dev) {
		/* must be device enumeration, add to the device space */
		DOCA_LOG_DBG("ENUMERATION CFG read ext_reg_num %d mask 0x%x", ext_reg_num, be_mask);
		if (ext_reg_num != 0) {
			DOCA_LOG_ERR("expected enumeration request (ext reg = 0)");
			DOCA_LOG_ERR("CFG read ext_reg_num %d mask 0x%x", ext_reg_num, be_mask);
			goto fatal_error;
		}

		virtio_dev = vblk_pci_dev_add(tlp_req);
		if (!virtio_dev)
			goto fatal_error;
	}

	dw_mask = vblk_pci_bit_enable_to_mask(be_mask);
	tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	dw_reg = pcie_config_read(TO_PCIE_RAW_CFG(&virtio_dev->pcie_dev), ext_reg_num);
	completion_data = dw_reg & dw_mask;

	DOCA_LOG_DBG("CFG read result: 0x%x & 0x%x -> 0x%x", dw_reg, dw_mask, completion_data);

	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, 1);

	memcpy(tlp_cpl_data, &completion_data, sizeof(completion_data));

	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
	return;

fatal_error:
	vblk_pci_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
}

static void vblk_pci_handle_mmio_read(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	const uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);

	/* lookup device by address */
	uint32_t *dw_header = (uint32_t *)tlp_req_header, *tlp_cpl_data;
	uint64_t addr;
	uint32_t dw_mask, bit_en_mask;
	int byte_count;

	if (fmt == TLP_FMT_4DW_NO_DATA)
		addr = ((uint64_t)be32toh(dw_header[2]) << 32) | (be32toh(dw_header[3]) & ~0x3ULL);
	else
		addr = be32toh(dw_header[2]) & ~0x3;

	DOCA_LOG_DBG("MMIO read 0x%lx len %d", addr, length);

	/* requests are routed by the mmio address */
	struct vblk_pci_virtio_dev *virtio_dev = vblk_pci_dev_find_by_addr(addr);
	if (!virtio_dev) {
		DOCA_LOG_ERR("fatal invalid mmio address 0x%lx", addr);
		return doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, NULL);
	}

	/* at the moment we only support 1 & 2 dwords reads */
	if (length > 2) {
		DOCA_LOG_ERR("unsupported read length %d", length);
		goto fatal_error;
	}

	if (length == 0) {
		DOCA_LOG_DBG("zero read %d", length);
		goto zero_read;
	}

	tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	byte_count = 4 * length;

	bit_en_mask = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
	byte_count -= (4 - __builtin_popcount(bit_en_mask));
	tlp_cpl_data[0] = (vblk_pci_virtio_mmio_read32(virtio_dev, addr) & dw_mask);

	DOCA_LOG_DBG("MMIO read result: 0x%lx result 0x%x mask 0x%x", addr, tlp_cpl_data[0], dw_mask);

	if (length == 2) {
		bit_en_mask = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
		dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
		byte_count -= (4 - __builtin_popcount(bit_en_mask));
		tlp_cpl_data[1] = (vblk_pci_virtio_mmio_read32(virtio_dev, addr + 4) & dw_mask);
	}

	vblk_pci_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_WITH_DATA, TLP_CPL_STATUS_SC, length, byte_count);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
	return;

fatal_error:
	vblk_pci_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_UR, 0, 4);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
	return;

zero_read:
	vblk_pci_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_NO_DATA, TLP_CPL_STATUS_SC, 0, 0);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, virtio_dev->pci_tlp_dev);
}

static void vblk_pci_handle_mmio_write(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	void *tlp_req_header = (void *)doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t *tlp_req_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);	/* data */
	const uint32_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	const uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);

	/* lookup device by address */
	uint32_t *dw_header = (uint32_t *)tlp_req_header;
	uint64_t addr;

	if (fmt == TLP_FMT_4DW_WITH_DATA)
		addr = ((uint64_t)be32toh(dw_header[2]) << 32) | (be32toh(dw_header[3]) & ~0x3ULL);
	else
		addr = be32toh(dw_header[2]) & ~0x3;

	DOCA_LOG_DBG("MMIO write 0x%lx len %d data1 0x%0x", addr, length, length > 0 ? tlp_req_data[0] : 0);

	/* requests are routed by the mmio address */
	struct vblk_pci_virtio_dev *virtio_dev = vblk_pci_dev_find_by_addr(addr);
	if (!virtio_dev) {
		DOCA_LOG_ERR("fatal invalid mmio address 0x%lx", addr);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, NULL);
		return;
	}

	/* at the moment we only support 1 & 2 dwords writes */
	if (length > 2) {
		DOCA_LOG_ERR("unsupported write length %d", length);
		goto done;
	}

	if (length == 0) {
		DOCA_LOG_ERR("zero read %d", length);
		goto done;
	}

	uint32_t dw_mask;
	uint32_t bit_en_mask;

	bit_en_mask = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
	vblk_pci_virtio_mmio_write32(virtio_dev, addr, tlp_req_data[0], dw_mask);

	if (length == 2) {
		bit_en_mask = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
		dw_mask = vblk_pci_bit_enable_to_mask(bit_en_mask);
		vblk_pci_virtio_mmio_write32(virtio_dev, addr + 4, tlp_req_data[1], dw_mask);
	}

done:
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, virtio_dev->pci_tlp_dev);
}

static void vblk_pci_handle_pci_event(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	enum doca_devemu_pci_tlp_channel_req_pci_event_opmode pci_event_opmode =
		doca_devemu_pci_tlp_channel_req_get_pci_event_opmode(tlp_req);
	switch (pci_event_opmode) {
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_ASSERT:
		DOCA_LOG_INFO("PERST# is asserted (enters reset)");
		break;
	case DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_PCI_EVENT_OPMODE_PERST_DEASSERT:
		DOCA_LOG_INFO("PERST# is deasserted (released from reset)");
		break;
	default:
		DOCA_LOG_WARN("Unknown PCI_EVENT opmode: %d", pci_event_opmode);
		break;
	}

	/* Complete the PCI_EVENT request */
	doca_devemu_pci_tlp_channel_req_complete_pci_event(tlp_req);
}

static void vblk_pci_ev_cb(struct doca_devemu_pci_tlp_channel *channel,
			   struct doca_devemu_pci_tlp_channel_req *tlp_req,
			   void *req_user_data)
{
	DOCA_LOG_DBG("--- New TLP");
	enum doca_devemu_pci_tlp_channel_req_opcode opcode = doca_devemu_pci_tlp_channel_req_get_opcode(tlp_req);
	enum tlp_req_type type;

	(void)channel;
	(void)req_user_data;

	if (doca_unlikely(opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_PCI_EVENT)) {
		return vblk_pci_handle_pci_event(tlp_req);
	}

	type = tlp_req_get_type(tlp_req);
	switch (type) {
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_0:
		vblk_pci_handle_cfg_read0(tlp_req);
		return;
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0:
		vblk_pci_handle_cfg_write0(tlp_req);
		return;
	case TLP_REQ_TYPE_MEMORY_READ:
		vblk_pci_handle_mmio_read(tlp_req);
		return;
	case TLP_REQ_TYPE_MEMORY_WRITE:
		vblk_pci_handle_mmio_write(tlp_req);
		return;
	default:
		DOCA_LOG_ERR("TLP type %d is not supported", type);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, NULL);
	}
}

static doca_error_t vblk_pci_ev_channel_init(struct doca_dev *dev)
{
	doca_error_t err;

	err = doca_devemu_pci_tlp_channel_create(dev, &vblk_pci_ev_channel);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create even channel");
		return err;
	}

	err = doca_devemu_pci_tlp_channel_event_req_register(vblk_pci_ev_channel, vblk_pci_ev_cb);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register event callback");
		doca_devemu_pci_tlp_channel_destroy(vblk_pci_ev_channel);
		return err;
	}

	return DOCA_SUCCESS;
}

struct doca_ctx *vblk_pci_ev_channel_ctx(void)
{
	return doca_devemu_pci_tlp_channel_as_ctx(vblk_pci_ev_channel);
}

static void vblk_pci_ev_channel_reset(void)
{
	doca_devemu_pci_tlp_channel_destroy(vblk_pci_ev_channel);
}

doca_error_t vblk_pci_tlp_start(void)
{
	doca_error_t err;

	err = doca_ctx_start(vblk_pci_ev_channel_ctx());
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start vblk_pci event channel");
		return err;
	}

	/* The single-USP topology (1 USP + N DSP + N EP) requires
	 * exactly one physical NV switch TLP downstream port.
	 * N (emulated endpoints from -n) is unaffected by this check. */
	uint8_t num_nv_switch_tlp_dsp = 0;

	err = doca_devemu_pci_tlp_channel_get_num_dsp(vblk_pci_ev_channel, &num_nv_switch_tlp_dsp);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get num_dsp: %s", doca_error_get_descr(err));
		goto stop;
	}

	if (num_nv_switch_tlp_dsp != 1) {
		DOCA_LOG_ERR("Expected 1 NV switch TLP DSP, got %u; "
			     "use mlxconfig to set TLP ports to 1",
			     num_nv_switch_tlp_dsp);
		err = DOCA_ERROR_NOT_SUPPORTED;
		goto stop;
	}

	return DOCA_SUCCESS;

stop:
	doca_ctx_stop(vblk_pci_ev_channel_ctx());
	return err;
}

void vblk_pci_stop(void)
{
	doca_error_t err;

	err = doca_ctx_stop(vblk_pci_ev_channel_ctx());
	/* consider: handle in progress either sync or async */
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop vblk_pci event channel");
	}
}

void vblk_pci_reset(void)
{
	vblk_pci_virtio_reset();
	vblk_pci_ev_channel_reset();
}

doca_error_t vblk_pci_type_init(struct doca_dev *dev)
{
	doca_error_t err;
	uint16_t n_tlp_types;

	err = doca_devemu_pci_tlp_cap_get_max_types(doca_dev_as_devinfo(dev), &n_tlp_types);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query TLP emulation support");
		return err;
	}

	DOCA_LOG_INFO("device supports %d pci tlp types", n_tlp_types);
	if (n_tlp_types == 0) {
		DOCA_LOG_ERR("device has no TLP emulation support");
		return DOCA_ERROR_NOT_SUPPORTED;
	}

	err = vblk_pci_virtio_block_init(dev);
	return err;
}

void vblk_pci_type_reset(void)
{
	vblk_pci_virtio_reset();
}

doca_error_t vblk_pci_init(struct doca_dev *dev)
{
	doca_error_t err;
	uint16_t n_tlp_types;

	err = doca_devemu_pci_tlp_cap_get_max_types(doca_dev_as_devinfo(dev), &n_tlp_types);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query TLP emulation support");
		return err;
	}

	DOCA_LOG_INFO("device supports %d pci tlp types", n_tlp_types);
	if (n_tlp_types == 0) {
		DOCA_LOG_ERR("device has no TLP emulation support");
		return err;
	}

	/* create channel for polling */
	err = vblk_pci_ev_channel_init(dev);
	if (err != DOCA_SUCCESS)
		return err;

	err = vblk_pci_virtio_block_init(dev);
	if (err != DOCA_SUCCESS)
		goto reset_ev_channel;

	return DOCA_SUCCESS;

reset_ev_channel:
	vblk_pci_ev_channel_reset();
	return err;
}

struct vblk_pci_virtio_dev *vblk_pci_virtio_dev_create(const struct vblk_pci_virtio_attrs *attr)
{
	struct vblk_pci_virtio_dev *dev;
	doca_error_t err;

	if (g_vblk_pci_dev != NULL) {
		DOCA_LOG_ERR("Only one vblock pci device is supported");
		return NULL;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	err = doca_devemu_pci_type_create_rep(vblk_pci_virtio_dev_type, &dev->dev_rep);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create PCI TLP representor: %s", doca_error_get_descr(err));
		goto free_mem;
	}

	err = doca_devemu_pci_tlp_dev_create(vblk_pci_virtio_dev_type, dev->dev_rep, &dev->pci_tlp_dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create TLP device: %s", doca_error_get_descr(err));
		goto destroy_rep;
	}

	err = vblk_pci_virtio_dev_init(dev, attr);
	if (err != DOCA_SUCCESS)
		goto destroy_dev;

	err = doca_devemu_pci_tlp_dev_start(dev->pci_tlp_dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to start TLP device: %s", doca_error_get_descr(err));
		goto destroy_dev;
	}

	dev->owns_rep = true;
	g_vblk_pci_dev = dev;
	return dev;

destroy_dev:
	if (DOCA_SUCCESS != doca_devemu_pci_tlp_dev_destroy(dev->pci_tlp_dev)) {
		DOCA_LOG_ERR("Failed to destroy pci_tlp_dev: %p", dev->pci_tlp_dev);
	}
destroy_rep:
	if (DOCA_SUCCESS != doca_devemu_pci_type_destroy_rep(dev->dev_rep)) {
		DOCA_LOG_ERR("Failed to destroy pci_tlp_dev rep: %p", dev->dev_rep);
	}
free_mem:
	free(dev);
	return NULL;
}

struct vblk_pci_virtio_dev *vblk_pci_virtio_dev_open(uint16_t num_queues)
{
	struct vblk_pci_virtio_dev *dev;
	struct doca_devinfo_rep **rep_list;
	uint32_t nb_reps;
	doca_error_t err;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	/* Discover existing rep (created by TLP app) */
	err = doca_devemu_pci_type_create_rep_list(vblk_pci_virtio_dev_type, &rep_list, &nb_reps);
	if (err != DOCA_SUCCESS || nb_reps == 0) {
		DOCA_LOG_ERR("No existing representor found (nb_reps=%u): %s", nb_reps, doca_error_get_descr(err));
		goto free_mem;
	}

	err = doca_dev_rep_open(rep_list[0], &dev->dev_rep);
	doca_devinfo_rep_destroy_list(rep_list);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open existing representor: %s", doca_error_get_descr(err));
		goto free_mem;
	}

	err = doca_devemu_pci_tlp_dev_create_started(vblk_pci_virtio_dev_type, dev->dev_rep, &dev->pci_tlp_dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to create started TLP device: %s", doca_error_get_descr(err));
		goto close_rep;
	}

	dev->owns_rep = false;
	dev->pci_cfg.num_queues = num_queues;
	g_vblk_pci_dev = dev;
	DOCA_LOG_INFO("Opened existing PCI endpoint (num_queues=%u)", num_queues);
	return dev;

close_rep:
	doca_dev_rep_close(dev->dev_rep);
free_mem:
	free(dev);
	return NULL;
}

void vblk_pci_virtio_dev_destroy(struct vblk_pci_virtio_dev *dev)
{
	if (dev->is_enumerated)
		vblk_pci_space[dev->bus][dev->device][dev->function] = NULL;

	if (g_vblk_pci_dev->msix && DOCA_SUCCESS != doca_devemu_pci_msix_destroy(g_vblk_pci_dev->msix)) {
		DOCA_LOG_ERR("Failed to destroy msix: %p", g_vblk_pci_dev->msix);
	}
	g_vblk_pci_dev = NULL;

	if (DOCA_SUCCESS != doca_devemu_pci_tlp_dev_stop(dev->pci_tlp_dev)) {
		DOCA_LOG_ERR("Failed to stop pci_tlp_dev: %p", dev->pci_tlp_dev);
	}

	if (DOCA_SUCCESS != doca_devemu_pci_tlp_dev_destroy(dev->pci_tlp_dev)) {
		DOCA_LOG_ERR("Failed to destroy pci_tlp_dev: %p", dev->pci_tlp_dev);
	}

	if (dev->owns_rep) {
		if (DOCA_SUCCESS != doca_devemu_pci_type_destroy_rep(dev->dev_rep))
			DOCA_LOG_ERR("Failed to destroy rep: %p", dev->dev_rep);
	} else {
		doca_dev_rep_close(dev->dev_rep);
	}

	free(dev);
}
