/*
 * Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include "spdk/nvmf_transport.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include <spdk/nvme_spec.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_pe.h>
#include <doca_transport_common.h>

#include "nvme_pci_common.h"
#include "nvme_pci_type_config.h"
#include "nvmf_doca_io.h"

DOCA_LOG_REGISTER(NVME_EMULATION_DOCA_TRANSPORT);

#define HOTPLUG_TIMEOUT_IN_MICROS (5 * 1000 * 1000) /* Set timeout to 5 seconds  */

#define NVMF_DOCA_DEFAULT_MAX_QUEUE_DEPTH 512
#define NVMF_DOCA_DEFAULT_MAX_QPAIRS_PER_CTRLR 128
#define NVMF_DOCA_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define NVMF_DOCA_DEFAULT_MAX_IO_SIZE 131072
#define NVMF_DOCA_DEFAULT_IO_UINT_SIZE 128
#define NVMF_DOCA_DEFAULT_AQ_DEPTH 256
#define NVMF_DOCA_DEFAULT_NUM_SHARED_BUFFER 1
#define NVMF_DOCA_DEFAULT_BUFFER_CACHE_SIZE 0
#define NVMF_DOCA_DIF_INSERT_OR_STRIP false
#define NVMF_DOCA_DEFAULT_ABORT_TIMEOUT_SEC 1

#define NVMF_ADMIN_QUEUE_ID 0
#define ADMIN_QP_POLL_RATE_LIMIT 1000

/*
 * A for-each loop that allows a node to be removed or freed within the loop.
 */
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar) \
	for ((var) = ((head)->tqh_first); (var) && ((tvar) = ((var)->field.tqe_next), 1); (var) = (tvar))
#endif

/*
 * A struct that includes all needed info on registered kernels and is initialized during linkage by DPACC
 * Variable name should be the token passed to DPACC with --app-name parameter
 */
extern struct doca_dpa_app *nvmf_doca_transport_app;

struct nvmf_doca_nvme_registers {
	/** controller capabilities */
	union spdk_nvme_cap_register cap;

	/** version of NVMe specification */
	union spdk_nvme_vs_register vs;
	uint32_t intms; /* interrupt mask set */
	uint32_t intmc; /* interrupt mask clear */

	/** controller configuration */
	union spdk_nvme_cc_register cc;

	uint32_t reserved1;
	union spdk_nvme_csts_register csts; /* controller status */
	uint32_t nssr;			    /* NVM subsystem reset */

	/** admin queue attributes */
	union spdk_nvme_aqa_register aqa;

	uint64_t asq; /* admin submission queue base addr */
	uint64_t acq; /* admin completion queue base addr */
};

enum nvmf_doca_listener_state {
	NVMF_DOCA_LISTENER_UNINITIALIZED,
	NVMF_DOCA_LISTENER_INITIALIZING,
	NVMF_DOCA_LISTENER_INITIALIZATION_ERROR,
	NVMF_DOCA_LISTENER_INITIALIZED,
	NVMF_DOCA_LISTENER_RESETTING,
};

struct nvmf_doca_emulation_manager {
	struct doca_dev *emulation_manager;	       /**< Emulation manager */
	struct doca_devemu_pci_type *pci_type;	       /**< PCI type */
	struct doca_dpa *dpa;			       /**< Doca DPA */
	TAILQ_ENTRY(nvmf_doca_emulation_manager) link; /**< Link to next emulation manager context */
};

struct nvmf_doca_admin_qp {
	struct nvmf_doca_io *admin_cq;	   /**< Admin CQ if exists, NULL otherwise */
	struct nvmf_doca_sq *admin_sq;	   /**< Admin CQ if exists, NULL otherwise */
	TAILQ_HEAD(, nvmf_doca_io) io_cqs; /**< CQ list */
	TAILQ_HEAD(, nvmf_doca_sq) io_sqs; /**< SQ list */
	bool stopping_all_io_cqs;
};

struct nvmf_doca_pci_dev_admin;

struct nvmf_doca_pci_dev_poll_group {
	struct doca_mmap *host_mmap;			/**< Host mmap */
	struct nvmf_doca_pci_dev_admin *pci_dev_admin;	/**< The PCI device admin context */
	struct nvmf_doca_admin_qp *admin_qp;		/**< The PCI device admin QP context. Can be NULL */
	TAILQ_HEAD(, nvmf_doca_io) io_cqs;		/**< CQ list */
	struct nvmf_doca_poll_group *poll_group;	/**< The parent poll group */
	TAILQ_ENTRY(nvmf_doca_pci_dev_poll_group) link; /**< Link to next pci dev poll group */
};

struct nvmf_doca_poll_group {
	struct spdk_nvmf_transport_poll_group pg; /**< NVMF transport poll group */
	struct doca_pe *pe;			  /**< Doca progress engine */
	struct doca_pe *admin_qp_pe;		  /**< Doca admin QP progress engine*/
	size_t admin_qp_poll_rate_limiter;	  /**< Counter to limit the frequency of admin QP polling */
	TAILQ_HEAD(, nvmf_doca_pci_dev_poll_group) pci_dev_pg_list; /**< PCI dev poll group list */
	TAILQ_ENTRY(nvmf_doca_poll_group) link;			    /**< Link to next poll group */
};

struct nvmf_doca_admin_poll_group;

struct nvmf_doca_pci_dev_admin {
	struct nvmf_doca_transport *doca_transport;	       /**< Doca transport */
	struct nvmf_doca_emulation_manager *emulation_manager; /**< Emulation manager */
	struct doca_devemu_pci_dev *pci_dev;		       /**< PCI device */
	struct spdk_nvmf_subsystem *subsystem;		       /**< NVMF subsystem */
	struct doca_dev_rep *dev_rep;			       /**< Device representor */
	struct spdk_nvme_transport_id trid;		       /**< Transport ID */
	enum nvmf_doca_listener_state state;		       /**< Doca listener state */
	struct spdk_nvmf_ctrlr *ctrlr;			       /**< NVMF controller */
	void *stateful_region_values;			       /**< Buffer used to query stateful region values */
	struct nvmf_doca_admin_qp *admin_qp;		       /**< Admin QP context */
	struct nvmf_doca_poll_group *admin_qp_pg;	       /**< Poll group associated with admin QP */
	bool is_flr;					       /**< Flag to indicate if an FLR event has occured */
	bool is_destroy_flow;				       /**< Indicates if PCI device should be destroyed */
	uint32_t ctlr_id;
	TAILQ_ENTRY(nvmf_doca_pci_dev_admin) link; /**< Link to next device context */
};

struct nvmf_doca_admin_poll_group {
	struct doca_pe *pe;				      /**< Used by poller */
	struct spdk_poller *poller;			      /**< Used to poll PCI devs and admin QPs */
	struct spdk_thread *thread;			      /**< The thread where poller is running */
	TAILQ_HEAD(, nvmf_doca_pci_dev_admin) pci_dev_admins; /**< PCI devices list */
};

struct nvmf_doca_transport {
	struct spdk_nvmf_transport transport;			      /**< NVMF transport */
	TAILQ_HEAD(, nvmf_doca_emulation_manager) emulation_managers; /**< Emulation managers list */
	TAILQ_HEAD(, nvmf_doca_poll_group) poll_groups;		      /**< Doca poll group list */
	struct nvmf_doca_poll_group *last_selected_pg;		      /**< Last selected poll group for round robin */
	struct nvmf_doca_admin_poll_group admin_pg;		      /**< Used to poll PCI devs and admin QPs */
	uint32_t num_of_listeners; /**< The number of listeners belongs to the transport*/
};

/* Static functions forward declarations */
#define NVME_PAGE_SIZE 4096

static void post_cqe_from_response(struct nvmf_doca_request *request, void *arg);
static void nvmf_doca_on_post_cqe_complete(struct nvmf_doca_cq *cq, union doca_data user_data);
static void nvmf_doca_on_fetch_sqe_complete(struct nvmf_doca_sq *sq, struct nvmf_doca_sqe *sqe, uint16_t sqe_idx);
static void nvmf_doca_on_copy_data_complete(struct nvmf_doca_sq *sq,
					    struct doca_buf *dst,
					    struct doca_buf *src,
					    union doca_data user_data);
static void nvmf_doca_on_post_nvm_cqe_complete(struct nvmf_doca_cq *cq, union doca_data user_data);
static void nvmf_doca_on_fetch_nvm_sqe_complete(struct nvmf_doca_sq *sq, struct nvmf_doca_sqe *sqe, uint16_t sqe_idx);
static void nvmf_doca_on_copy_nvm_data_complete(struct nvmf_doca_sq *sq,
						struct doca_buf *dst,
						struct doca_buf *src,
						union doca_data user_data);
static void nvmf_doca_pci_dev_admin_reset_continue(struct nvmf_doca_pci_dev_admin *pci_dev_admin);
static void handle_controller_register_events(struct doca_devemu_pci_dev *pci_dev,
					      const struct bar_region_config *config);
static void nvmf_doca_destroy_pci_dev_poll_group(struct nvmf_doca_pci_dev_poll_group *pci_dev_pg);
static void nvmf_doca_destroy_admin_qp_continue(struct nvmf_doca_pci_dev_admin *pci_dev_admin);
static void nvmf_doca_on_initialization_error(void *cb_arg);
static void nvmf_doca_on_admin_sq_stop(struct nvmf_doca_sq *sq);
static doca_error_t nvmf_doca_create_host_mmap(struct doca_devemu_pci_dev *pci_dev,
					       struct doca_dev *emulation_manager,
					       struct doca_mmap **mmap_out);
static void buffers_ready_copy_data_dpu_to_host(struct nvmf_doca_request *request);
static void buffers_ready_copy_data_host_to_dpu(struct nvmf_doca_request *request);
static void nvmf_doca_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	opts->max_queue_depth = NVMF_DOCA_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr = NVMF_DOCA_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size = NVMF_DOCA_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size = NVMF_DOCA_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size = NVMF_DOCA_DEFAULT_IO_UINT_SIZE;
	opts->max_aq_depth = NVMF_DOCA_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers = NVMF_DOCA_DEFAULT_NUM_SHARED_BUFFER;
	opts->buf_cache_size = NVMF_DOCA_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip = NVMF_DOCA_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec = NVMF_DOCA_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->transport_specific = NULL;
}

/*
 * Selects a poll group from the system using the round-robin method
 *
 * @transport [in]: The doca transport that holds all the poll groups
 * @return: the selected poll group
 */
static struct nvmf_doca_poll_group *choose_poll_group(struct nvmf_doca_transport *transport)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_poll_group *poll_group;

	if (transport->last_selected_pg == NULL) {
		transport->last_selected_pg = TAILQ_FIRST(&transport->poll_groups);
	} else {
		transport->last_selected_pg = TAILQ_NEXT(transport->last_selected_pg, link);
	}

	if (transport->last_selected_pg == NULL) {
		transport->last_selected_pg = TAILQ_FIRST(&transport->poll_groups);
	}

	poll_group = transport->last_selected_pg;
	return poll_group;
}

/*
 * Finds the matching nvmf_doca_pci_dev_poll_group by the given PCI device
 *
 * @doca_poll_group [in]: The poll group that includes the nvmf_doca_pci_dev_poll_group
 * @pci_dev [in]: The PCI device
 * @return: A pointer to the nvmf_doca_pci_dev_poll_group matching if found, or null if not
 */
static struct nvmf_doca_pci_dev_poll_group *get_pci_dev_poll_group(struct nvmf_doca_poll_group *doca_poll_group,
								   struct doca_devemu_pci_dev *pci_dev)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg;

	TAILQ_FOREACH(pci_dev_pg, &doca_poll_group->pci_dev_pg_list, link)
	{
		if (pci_dev_pg->pci_dev_admin->pci_dev == pci_dev)
			return pci_dev_pg;
	}
	return NULL;
}

/*
 * Destroys emulation manager
 *
 * @doca_emulation_manager [in]: The emulation anager context
 * @return: DOCA_SUCCESS on success and other error code otherwise
 */
static doca_error_t nvmf_doca_destroy_emulation_manager(struct nvmf_doca_emulation_manager *doca_emulation_manager)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	doca_error_t ret;

	if (doca_emulation_manager->dpa != NULL) {
		ret = doca_dpa_stop(doca_emulation_manager->dpa);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop dpa: %s", doca_error_get_name(ret));
			return ret;
		}
		ret = doca_dpa_destroy(doca_emulation_manager->dpa);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy dpa: %s", doca_error_get_name(ret));
			return ret;
		}
	}

	cleanup_pci_resources(doca_emulation_manager->pci_type, doca_emulation_manager->emulation_manager);

	free(doca_emulation_manager);
	return DOCA_SUCCESS;
}

/*
 * Creates and starts a pci type
 *
 * @doca_emulation_manager [in]: The emulation manager
 * @return: DOCA_SUCCESS on success and other error code otherwise
 */
static doca_error_t nvmf_doca_pci_type_create_and_start(struct nvmf_doca_emulation_manager *doca_emulation_manager)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	const struct bar_memory_layout_config *layout_config;
	const struct bar_db_region_config *db_config;
	const struct bar_region_config *region_config;
	int idx;
	doca_error_t ret;

	ret = doca_devemu_pci_type_create(NVME_TYPE_NAME, &doca_emulation_manager->pci_type);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pci type: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = doca_devemu_pci_type_set_dev(doca_emulation_manager->pci_type, doca_emulation_manager->emulation_manager);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set device for pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	ret = doca_devemu_pci_type_set_device_id(doca_emulation_manager->pci_type, PCI_TYPE_DEVICE_ID);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set device ID for pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	ret = doca_devemu_pci_type_set_vendor_id(doca_emulation_manager->pci_type, PCI_TYPE_VENDOR_ID);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set vendor ID for pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	ret = doca_devemu_pci_type_set_subsystem_id(doca_emulation_manager->pci_type, PCI_TYPE_SUBSYSTEM_ID);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set subsystem ID for pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	ret = doca_devemu_pci_type_set_subsystem_vendor_id(doca_emulation_manager->pci_type,
							   PCI_TYPE_SUBSYSTEM_VENDOR_ID);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set subsystem vendor ID for the given pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	ret = doca_devemu_pci_type_set_revision_id(doca_emulation_manager->pci_type, PCI_TYPE_REVISION_ID);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set revision ID for pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	ret = doca_devemu_pci_type_set_class_code(doca_emulation_manager->pci_type, PCI_TYPE_CLASS_CODE);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set class code for pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	ret = doca_devemu_pci_type_set_num_msix(doca_emulation_manager->pci_type, PCI_TYPE_NUM_MSIX);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set the number of MSI-X for pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MEMORY_LAYOUT; ++idx) {
		layout_config = &layout_configs[idx];
		ret = doca_devemu_pci_type_set_memory_bar_conf(doca_emulation_manager->pci_type,
							       layout_config->bar_id,
							       layout_config->log_size,
							       layout_config->memory_type,
							       layout_config->prefetchable);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set layout at index %d: %s", idx, doca_error_get_name(ret));
			goto destroy_pci_type;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_DB_REGIONS; ++idx) {
		db_config = &db_configs[idx];
		if (db_config->with_data)
			ret = doca_devemu_pci_type_set_bar_db_region_by_data_conf(doca_emulation_manager->pci_type,
										  db_config->region.bar_id,
										  db_config->region.start_address,
										  db_config->region.size,
										  db_config->log_db_size,
										  db_config->db_id_msbyte,
										  db_config->db_id_lsbyte);
		else
			ret = doca_devemu_pci_type_set_bar_db_region_by_offset_conf(doca_emulation_manager->pci_type,
										    db_config->region.bar_id,
										    db_config->region.start_address,
										    db_config->region.size,
										    db_config->log_db_size,
										    db_config->log_db_stride_size);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set DB region at index %d: %s", idx, doca_error_get_name(ret));
			goto destroy_pci_type;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_TABLE_REGIONS; ++idx) {
		region_config = &msix_table_configs[idx];
		ret = doca_devemu_pci_type_set_bar_msix_table_region_conf(doca_emulation_manager->pci_type,
									  region_config->bar_id,
									  region_config->start_address,
									  region_config->size);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X table region at index %d: %s", idx, doca_error_get_name(ret));
			goto destroy_pci_type;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_MSIX_PBA_REGIONS; ++idx) {
		region_config = &msix_pba_configs[idx];
		ret = doca_devemu_pci_type_set_bar_msix_pba_region_conf(doca_emulation_manager->pci_type,
									region_config->bar_id,
									region_config->start_address,
									region_config->size);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set MSI-X pending bit array region at index %d: %s",
				     idx,
				     doca_error_get_name(ret));
			goto destroy_pci_type;
		}
	}

	for (idx = 0; idx < PCI_TYPE_NUM_BAR_STATEFUL_REGIONS; ++idx) {
		region_config = &stateful_configs[idx];
		ret = doca_devemu_pci_type_set_bar_stateful_region_conf(doca_emulation_manager->pci_type,
									region_config->bar_id,
									region_config->start_address,
									region_config->size);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to set Stateful region at index %d: %s", idx, doca_error_get_name(ret));
			goto destroy_pci_type;
		}
	}

	ret = doca_devemu_pci_type_start(doca_emulation_manager->pci_type);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start pci type: %s", doca_error_get_name(ret));
		goto destroy_pci_type;
	}

	uint8_t data[128] = {};
	struct nvmf_doca_nvme_registers *registers = (struct nvmf_doca_nvme_registers *)&data[0];
	*registers = (struct nvmf_doca_nvme_registers){
		.cap.bits =
			{
				.mqes = NVMF_DOCA_DEFAULT_MAX_QUEUE_DEPTH - 1,
				.cqr = 0x1,
				.to = 0xf0,
				.css = 0x1,
			},
		.vs.bits =
			{
				.mjr = 0x1,
				.mnr = 0x3,
			},
	};
	ret = doca_devemu_pci_type_modify_bar_stateful_region_default_values(doca_emulation_manager->pci_type,
									     0,
									     0,
									     data,
									     sizeof(data));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify bar stateful region: %s", doca_error_get_name(ret));
		doca_devemu_pci_type_stop(doca_emulation_manager->pci_type);
		return ret;
	}

	return ret;

destroy_pci_type:
	doca_devemu_pci_type_destroy(doca_emulation_manager->pci_type);
	return ret;
}

/*
 * Creates emulation manager context
 *
 * @dev_info [in]: The device info
 * @ret_emulation_manager [out]: The returned emulation manager context
 * @return: DOCA_SUCCESS on success and other error code otherwise
 */
static doca_error_t nvmf_doca_create_emulation_manager(struct doca_devinfo *dev_info,
						       struct nvmf_doca_emulation_manager **ret_emulation_manager)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_emulation_manager *doca_emulation_manager;
	doca_error_t ret;

	doca_emulation_manager =
		(struct nvmf_doca_emulation_manager *)calloc(1, sizeof(struct nvmf_doca_emulation_manager));
	if (doca_emulation_manager == NULL) {
		DOCA_LOG_INFO("Failed to allocate memory for emultaion manager context");
		return DOCA_ERROR_NO_MEMORY;
	}

	ret = doca_dev_open(dev_info, &doca_emulation_manager->emulation_manager);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to open doca device: %s", doca_error_get_name(ret));
		nvmf_doca_destroy_emulation_manager(doca_emulation_manager);
		return ret;
	}

	ret = nvmf_doca_pci_type_create_and_start(doca_emulation_manager);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize PCI type: %s", doca_error_get_name(ret));
		nvmf_doca_destroy_emulation_manager(doca_emulation_manager);
		return ret;
	}

	ret = doca_dpa_create(doca_emulation_manager->emulation_manager, &doca_emulation_manager->dpa);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DPA context: %s", doca_error_get_name(ret));
		nvmf_doca_destroy_emulation_manager(doca_emulation_manager);
		return ret;
	}

	ret = doca_dpa_set_app(doca_emulation_manager->dpa, nvmf_doca_transport_app);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA app: %s", doca_error_get_name(ret));
		nvmf_doca_destroy_emulation_manager(doca_emulation_manager);
		return ret;
	}

	ret = doca_dpa_start(doca_emulation_manager->dpa);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DPA context: %s", doca_error_get_name(ret));
		nvmf_doca_destroy_emulation_manager(doca_emulation_manager);
		return ret;
	}

	*ret_emulation_manager = doca_emulation_manager;
	return ret;
}

static int nvmf_doca_admin_poll_group_poll(void *arg)
{
	struct nvmf_doca_admin_poll_group *admin_pg = arg;

	return doca_pe_progress(admin_pg->pe);
}

static void nvmf_doca_admin_poll_group_destroy(struct nvmf_doca_admin_poll_group *admin_pg)
{
	if (admin_pg->poller != NULL) {
		spdk_poller_unregister(&admin_pg->poller);
		admin_pg->poller = NULL;
	}

	doca_error_t ret = doca_pe_destroy(admin_pg->pe);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy admin progress engine: %s", doca_error_get_name(ret));
	}
}

static doca_error_t nvmf_doca_admin_poll_group_create(struct nvmf_doca_admin_poll_group *admin_pg)
{
	doca_error_t ret;

	ret = doca_pe_create(&admin_pg->pe);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create admin progress engine: %s", doca_error_get_name(ret));
		return ret;
	}

	/* This poller will be used on application thread to poll PCI events */
	admin_pg->poller = spdk_poller_register(nvmf_doca_admin_poll_group_poll, admin_pg, 0);
	if (admin_pg->poller == NULL) {
		DOCA_LOG_ERR("Failed to register admin poller");
		doca_pe_destroy(admin_pg->pe);
		return DOCA_ERROR_INITIALIZATION;
	}

	TAILQ_INIT(&admin_pg->pci_dev_admins);

	admin_pg->thread = spdk_get_thread();
	assert(admin_pg->thread == spdk_thread_get_app_thread());

	return DOCA_SUCCESS;
}

/*
 * Creates the DOCA transport
 *
 * Callback invoked by the NVMf target once user issues the create transport RPC
 * The callback is invoked after the nvmf_doca_opts_init callback
 *
 * @opts [in]: The transport options
 * @return: The newly created DOCA transport on success and NULL otherwise
 */
static struct spdk_nvmf_transport *nvmf_doca_create(struct spdk_nvmf_transport_opts *opts)
{
	(void)opts;

	DOCA_LOG_DBG("Entering function %s", __func__);

	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	uint8_t is_hotplug_manager;
	doca_error_t ret;

	struct nvmf_doca_transport *doca_transport =
		(struct nvmf_doca_transport *)calloc(1, sizeof(struct nvmf_doca_transport));
	if (doca_transport == NULL) {
		DOCA_LOG_INFO("Failed to allocate memory for doca_transport");
		return NULL;
	}

	TAILQ_INIT(&doca_transport->poll_groups);
	TAILQ_INIT(&doca_transport->emulation_managers);
	doca_transport->last_selected_pg = NULL;

	ret = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Method doca_devinfo_create_list failed: %s", doca_error_get_name(ret));
		goto free_transport;
	}

	for (uint32_t idx = 0; idx < nb_devs; idx++) {
		ret = doca_devinfo_cap_is_hotplug_manager_supported(dev_list[idx], &is_hotplug_manager);
		if (ret == DOCA_SUCCESS && is_hotplug_manager == 1) {
			struct nvmf_doca_emulation_manager *doca_emulation_manager;

			ret = nvmf_doca_create_emulation_manager(dev_list[idx], &doca_emulation_manager);
			if (ret != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Emulation manager initialization failed: %s", doca_error_get_name(ret));
			} else {
				TAILQ_INSERT_TAIL(&doca_transport->emulation_managers, doca_emulation_manager, link);
			}
			break;
		}
	}

	if (TAILQ_EMPTY(&doca_transport->emulation_managers)) {
		DOCA_LOG_ERR("No emulation managers available");
		goto destroy_list;
	}

	ret = doca_devinfo_destroy_list(dev_list);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy devinfo list: %s", doca_error_get_name(ret));
		goto free_transport;
	}

	/* This poller will be used on application thread to poll PCI events */
	ret = nvmf_doca_admin_poll_group_create(&doca_transport->admin_pg);
	if (ret != DOCA_SUCCESS) {
		goto free_transport;
	}

	doca_transport->num_of_listeners = 0;

	return &doca_transport->transport;

destroy_list:
	ret = doca_devinfo_destroy_list(dev_list);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy devinfo list: %s", doca_error_get_name(ret));
	}
free_transport:
	free(doca_transport);

	return NULL;
}

/*
 * Dump transport-specific opts into JSON
 *
 * @transport [in]: The DOCA transport
 * @w [out]: The JSON dump
 */
static void nvmf_doca_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	(void)transport;
	(void)w;
}

/*
 * Destroys the DOCA transport
 *
 * Callback invoked by the NVMf target once user issues the destroy transport RPC
 *
 * @transport [in]: The DOCA transport to destroy
 * @cb_fn [in]: Callback to be invoked once destroy finished - can be NULL
 * @cb_arg [in]: Argument to be passed to the callback
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_destroy(struct spdk_nvmf_transport *transport,
			     spdk_nvmf_transport_destroy_done_cb cb_fn,
			     void *cb_arg)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_transport *doca_transport;
	struct nvmf_doca_emulation_manager *doca_emulation_manager, *temp;

	doca_transport = SPDK_CONTAINEROF(transport, struct nvmf_doca_transport, transport);

	nvmf_doca_admin_poll_group_destroy(&doca_transport->admin_pg);

	TAILQ_FOREACH_SAFE(doca_emulation_manager, &doca_transport->emulation_managers, link, temp)
	{
		TAILQ_REMOVE(&doca_transport->emulation_managers, doca_emulation_manager, link);
		nvmf_doca_destroy_emulation_manager(doca_emulation_manager);
	}

	free(doca_transport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
	return 0;
}

static struct nvmf_doca_pci_dev_admin *nvmf_doca_transport_find_pci_dev_admin(struct nvmf_doca_transport *doca_transport,
									      const char *vuid)
{
	struct nvmf_doca_pci_dev_admin *pci_dev_admin;

	TAILQ_FOREACH(pci_dev_admin, &doca_transport->admin_pg.pci_dev_admins, link)
	{
		if ((strncmp(vuid, pci_dev_admin->trid.traddr, DOCA_DEVINFO_REP_VUID_SIZE) == 0)) {
			return pci_dev_admin;
		}
	}

	return NULL;
}

/*
 * Checks if a PCI device with the given VUID exists within the specified transport.
 *
 * @doca_transport [in]: Doca transport.
 * @vuid [in]: VUID to look for.
 * @return: DOCA_SUCCESS on success and other error code otherwise
 */
static doca_error_t check_for_duplicate(struct nvmf_doca_transport *doca_transport, const char *vuid)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	return nvmf_doca_transport_find_pci_dev_admin(doca_transport, vuid) == NULL ? DOCA_SUCCESS :
										      DOCA_ERROR_NOT_PERMITTED;
}

/*
 * Finds an emulation manager and a fuction with the requested VUID
 *
 * @doca_transport [in]: Doca transport
 * @vuid [in]: vuid to look for
 * @ret_emulation_manager [out]: emulation manager
 * @ret_device_rep [out]: device rep
 * @return: DOCA_SUCCESS on success and other error code otherwise
 */
static doca_error_t find_emulation_manager_and_function_by_vuid(
	struct nvmf_doca_transport *doca_transport,
	const char *vuid,
	struct nvmf_doca_emulation_manager **ret_emulation_manager,
	struct doca_dev_rep **ret_device_rep)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_emulation_manager *doca_emulation_manager;
	struct doca_devinfo_rep **devinfo_list_rep;
	char rep_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	struct doca_dev_rep *device_rep;
	uint32_t nb_devs_rep;
	doca_error_t ret;

	TAILQ_FOREACH(doca_emulation_manager, &doca_transport->emulation_managers, link)
	{
		ret = doca_devemu_pci_type_create_rep_list(doca_emulation_manager->pci_type,
							   &devinfo_list_rep,
							   &nb_devs_rep);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Couldn't create the device representors list: %s", doca_error_get_name(ret));
			return ret;
		}

		for (uint32_t idx = 0; idx < nb_devs_rep; idx++) {
			ret = doca_devinfo_rep_get_vuid(devinfo_list_rep[idx], rep_vuid, DOCA_DEVINFO_REP_VUID_SIZE);
			if (ret == DOCA_SUCCESS && (strncmp(vuid, rep_vuid, DOCA_DEVINFO_REP_VUID_SIZE) == 0)) {
				ret = doca_dev_rep_open(devinfo_list_rep[idx], &device_rep);
				if (ret != DOCA_SUCCESS) {
					DOCA_LOG_ERR("Failed to open a device: %s", doca_error_get_name(ret));
					doca_devinfo_rep_destroy_list(devinfo_list_rep);
					return ret;
				}
				*ret_device_rep = device_rep;
				*ret_emulation_manager = doca_emulation_manager;
				doca_devinfo_rep_destroy_list(devinfo_list_rep);
				return DOCA_SUCCESS;
			}
		}
	}

	DOCA_LOG_ERR("Could not find an emulation manager and a fuction with the requested VUID");
	doca_devinfo_rep_destroy_list(devinfo_list_rep);
	return DOCA_ERROR_NOT_FOUND;
}

/*
 * Callback invoked once admin CQ has been stopped
 *
 * @io [in]: The NVMf DOCA IO that was stopped
 */
static void nvmf_doca_on_admin_cq_stop(struct nvmf_doca_io *io)
{
	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg = io->poll_group;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = pci_dev_pg->pci_dev_admin;
	struct nvmf_doca_admin_qp *admin_qp = pci_dev_pg->admin_qp;
	struct nvmf_doca_poll_group *doca_poll_group = pci_dev_pg->poll_group;

	nvmf_doca_io_destroy(io);
	free(io);
	admin_qp->admin_cq = NULL;

	DOCA_LOG_INFO("Destroying poll group %p PCI dev poll group %p", doca_poll_group, pci_dev_pg);
	TAILQ_REMOVE(&doca_poll_group->pci_dev_pg_list, pci_dev_pg, link);
	nvmf_doca_destroy_pci_dev_poll_group(pci_dev_pg);

	nvmf_doca_destroy_admin_qp_continue(pci_dev_admin);
}

/*
 * State changed callback
 *
 * @user_data [in]: Data user
 * @ctx [in]: Doca context
 * @prev_state [in]: Previous state
 * @next_state [in]: Next state
 */
static void devemu_state_changed_cb(const union doca_data user_data,
				    struct doca_ctx *ctx,
				    enum doca_ctx_states prev_state,
				    enum doca_ctx_states next_state)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	(void)ctx;
	(void)prev_state;

	struct nvmf_doca_pci_dev_admin *pci_dev_admin = user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		DOCA_LOG_DBG("DOCA_CTX_STATE_IDLE");
		if (!pci_dev_admin->is_destroy_flow) {
			doca_ctx_start(doca_devemu_pci_dev_as_ctx(pci_dev_admin->pci_dev));
		}
		pci_dev_admin->state = NVMF_DOCA_LISTENER_UNINITIALIZED;
		break;
	case DOCA_CTX_STATE_STARTING:
		DOCA_LOG_DBG("DOCA_CTX_STATE_STARTING");
		break;
	case DOCA_CTX_STATE_RUNNING:
		DOCA_LOG_DBG("DOCA_CTX_STATE_RUNNING");
		handle_controller_register_events(pci_dev_admin->pci_dev, &stateful_configs[0]);
		break;
	case DOCA_CTX_STATE_STOPPING:
		DOCA_LOG_DBG("Devemu device has entered into stopping state. Unexpected!, destroy datapath resources!");
		break;
	default:
		break;
	}
}

/*
 * message to stop the IO SQ
 *
 * Must be executed by poll group that owns the SQ. The Admin poll group will send this as message to owner of the SQ
 * This flow is async and once completed the nvmf_doca_pci_dev_poll_group_stop_io_sq_done() message will be sent
 * back to the admin poll group
 *
 * @ctx [in]: The context of the message
 */
static void nvmf_doca_pci_dev_poll_group_stop_io_sq(void *ctx)
{
	struct nvmf_doca_sq *sq = ctx;

	nvmf_doca_sq_stop(sq);
}

struct nvmf_doca_poll_group_delete_io_sq_ctx {
	struct nvmf_doca_request *request; /**< The original delete IO SQ admin command */
};

/*
 * Message to indicate that stopping of the IO SQ is done
 *
 * This message is used as a response to the nvmf_doca_pci_dev_poll_group_stop_io_sq() message
 * This message is sent to admin poll group once IO SQ has been stopped
 *
 * @ctx [in]: The context of the message
 */
static void nvmf_doca_pci_dev_poll_group_stop_io_sq_done(void *ctx)
{
	struct nvmf_doca_sq *sq = ctx;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = sq->io->poll_group->pci_dev_admin;
	struct nvmf_doca_admin_qp *admin_qp = pci_dev_admin->admin_qp;
	struct nvmf_doca_poll_group_delete_io_sq_ctx *delete_io_sq_ctx = sq->ctx;

	TAILQ_REMOVE(&admin_qp->io_sqs, sq, pci_dev_admin_link);
	free(sq);

	if (delete_io_sq_ctx == NULL) {
		/* Indicates that request to stop the IO SQ arrived from reset event */
		nvmf_doca_destroy_admin_qp_continue(pci_dev_admin);
	} else {
		/* Indicates that request to stop the IO SQ arrived delete IO SQ admin command */
		struct nvmf_doca_request *request = delete_io_sq_ctx->request;

		free(delete_io_sq_ctx);

		request->request.rsp->nvme_cpl.cid = request->request.cmd->nvme_cmd.cid;
		post_cqe_from_response(request, request);
	}
}

/*
 * Starts async flow of stopping all IO SQs each on their relevant poll group
 *
 * @admin_qp [in]: The PCI device admin QP context containing IO SQs from all poll groups
 */
static void nvmf_doca_admin_qp_stop_all_io_sqs(struct nvmf_doca_admin_qp *admin_qp)
{
	struct nvmf_doca_sq *sq;
	struct nvmf_doca_sq *sq_tmp;
	struct spdk_thread *thread;

	TAILQ_FOREACH_SAFE(sq, &admin_qp->io_sqs, pci_dev_admin_link, sq_tmp)
	{
		thread = sq->io->poll_group->poll_group->pg.group->thread;
		spdk_thread_exec_msg(thread, nvmf_doca_pci_dev_poll_group_stop_io_sq, sq);
	}
}

/*
 * message to stop the IO CQ
 *
 * Must be executed by poll group that owns the CQ. The Admin poll group will send this as message to owner of the CQ
 * This flow is async and once completed then the nvmf_doca_pci_dev_poll_group_stop_io_cq_done() message will be sent
 * back to the admin poll group
 *
 * @ctx [in]: The context of the message
 */
static void nvmf_doca_pci_dev_poll_group_stop_io_cq(void *ctx)
{
	struct nvmf_doca_io *io = ctx;

	nvmf_doca_io_stop(io);
}

struct nvmf_doca_poll_group_delete_io_cq_ctx {
	struct nvmf_doca_request *request; /**< The original delete IO SQ admin command */
};

/*
 * message to indicate that stopping of the IO CQ is done
 *
 * This message is used as a response to the nvmf_doca_pci_dev_poll_group_stop_io_cq() message
 * This message is sent to admin QP poll group once IO CQ has been stopped
 *
 * @ctx [in]: The context of the message
 */
static void nvmf_doca_pci_dev_poll_group_stop_io_cq_done(void *ctx)
{
	struct nvmf_doca_io *io = ctx;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = io->pci_dev_admin;
	struct nvmf_doca_admin_qp *admin_qp = pci_dev_admin->admin_qp;
	struct nvmf_doca_poll_group_delete_io_cq_ctx *delete_io_cq_ctx = io->ctx;

	TAILQ_REMOVE(&admin_qp->io_cqs, io, pci_dev_admin_link);
	free(io);

	if (delete_io_cq_ctx == NULL) {
		/* Indicates that request to stop the IO CQ arrived from reset event */
		nvmf_doca_destroy_admin_qp_continue(pci_dev_admin);
	} else {
		/* Indicates that request to stop the IO CQ arrived delete IO CQ admin command */
		struct nvmf_doca_request *request = delete_io_cq_ctx->request;

		free(delete_io_cq_ctx);

		request->request.rsp->nvme_cpl.cid = request->request.cmd->nvme_cmd.cid;
		post_cqe_from_response(request, request);
	}
}

/*
 * Starts async flow of stopping all IO CQs each on their relevant poll group
 *
 * @admin_qp [in]: The admin QP containing IO CQs from all poll groups
 */
static void nvmf_doca_admin_qp_stop_all_io_cqs(struct nvmf_doca_admin_qp *admin_qp)
{
	struct nvmf_doca_io *io;
	struct nvmf_doca_io *io_tmp;
	struct spdk_thread *thread;

	if (admin_qp->stopping_all_io_cqs)
		return;

	TAILQ_FOREACH_SAFE(io, &admin_qp->io_cqs, pci_dev_admin_link, io_tmp)
	{
		thread = io->poll_group->poll_group->pg.group->thread;
		spdk_thread_exec_msg(thread, nvmf_doca_pci_dev_poll_group_stop_io_cq, io);
	}

	admin_qp->stopping_all_io_cqs = true;
}

static void nvmf_doca_destroy_pci_dev_poll_group(struct nvmf_doca_pci_dev_poll_group *pci_dev_pg)
{
	doca_error_t ret;

	if (pci_dev_pg != NULL) {
		ret = doca_mmap_destroy(pci_dev_pg->host_mmap);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy PCI device poll group: Failed to destroy mmap - %s",
				     doca_error_get_name(ret));
		}
		pci_dev_pg->host_mmap = NULL;
	}
	DOCA_LOG_INFO("Destroyed PCI dev poll group %p", pci_dev_pg);
	memset(pci_dev_pg, 0, sizeof(*pci_dev_pg));
	free(pci_dev_pg);
}

/*
 * Creates a PCI device poll group object: nvmf_doca_pci_dev_poll_group
 *
 * @pci_dev_admin [in]: PCI device admin context
 * @admin_qp [in]: The admin QP context. Can be NULL in case this poll group does not poll the admin QP
 * @doca_poll_group [in]: Doca poll group
 * @ret_pci_dev_pg [out]: The newly created nvmf_doca_pci_dev_poll_group
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_create_pci_dev_poll_group(struct nvmf_doca_pci_dev_admin *pci_dev_admin,
							struct nvmf_doca_admin_qp *admin_qp,
							struct nvmf_doca_poll_group *doca_poll_group,
							struct nvmf_doca_pci_dev_poll_group **ret_pci_dev_pg)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg =
		(struct nvmf_doca_pci_dev_poll_group *)calloc(1, sizeof(*pci_dev_pg));
	if (pci_dev_pg == NULL) {
		DOCA_LOG_ERR("Failed to allocate doca device poll group");
		return DOCA_ERROR_NO_MEMORY;
	}

	pci_dev_pg->poll_group = doca_poll_group;
	pci_dev_pg->admin_qp = admin_qp;
	pci_dev_pg->pci_dev_admin = pci_dev_admin;
	TAILQ_INIT(&pci_dev_pg->io_cqs);

	doca_error_t ret = nvmf_doca_create_host_mmap(pci_dev_admin->pci_dev,
						      pci_dev_admin->emulation_manager->emulation_manager,
						      &pci_dev_pg->host_mmap);
	if (ret != DOCA_SUCCESS) {
		free(pci_dev_pg);
		return ret;
	}

	*ret_pci_dev_pg = pci_dev_pg;

	return DOCA_SUCCESS;
}

/*
 * message to indicate that destroy of the admin QP has finished
 *
 * This message is used as a response to the nvmf_doca_destroy_admin_qp() message
 * This message is sent to PCI device admin poll group once admin QP has been destroyed
 */
static void nvmf_doca_destroy_admin_qp_done(void *cb_arg)
{
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = cb_arg;

	pci_dev_admin->admin_qp = NULL;
	pci_dev_admin->admin_qp_pg = NULL;

	nvmf_doca_pci_dev_admin_reset_continue(pci_dev_admin);
}

/*
 * Continue async flow of destroying admin QP
 */
static void nvmf_doca_destroy_admin_qp_continue(struct nvmf_doca_pci_dev_admin *pci_dev_admin)
{
	struct nvmf_doca_admin_qp *admin_qp = pci_dev_admin->admin_qp;

	/* In case some IO CQs exist, then send message to all poll groups to delete their IO CQs */
	if (!TAILQ_EMPTY(&admin_qp->io_cqs)) {
		nvmf_doca_admin_qp_stop_all_io_cqs(admin_qp);
		return;
	}
	admin_qp->stopping_all_io_cqs = false;

	/* In case no IO CQs exist, then we can attempt to destroy the admin SQ */
	if (admin_qp->admin_sq != NULL) {
		nvmf_doca_sq_stop(admin_qp->admin_sq);
		return;
	}

	/* In case no admin SQ exist then we can destroy admin CQ */
	if (admin_qp->admin_cq != NULL) {
		nvmf_doca_io_stop(admin_qp->admin_cq);
		return;
	}

	free(admin_qp);

	spdk_thread_exec_msg(pci_dev_admin->doca_transport->admin_pg.thread,
			     nvmf_doca_destroy_admin_qp_done,
			     pci_dev_admin);
}

/*
 * Starts async flow of destroying admin QP
 *
 * This message is sent from PCI device admin thread to admin QP thread
 * Once admin QP is destroyed the admin QP thread will respond with nvmf_doca_destroy_admin_qp_done()
 */
static void nvmf_doca_destroy_admin_qp(void *cb_arg)
{
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = cb_arg;
	struct nvmf_doca_admin_qp *admin_qp = pci_dev_admin->admin_qp;

	/* In case some IO SQs exist, then send message to all poll groups to delete their IO SQs */
	if (!TAILQ_EMPTY(&admin_qp->io_sqs)) {
		nvmf_doca_admin_qp_stop_all_io_sqs(admin_qp);
		return;
	}

	nvmf_doca_destroy_admin_qp_continue(pci_dev_admin);
}

struct nvmf_doca_create_admin_qp_ctx {
	struct nvmf_doca_pci_dev_admin *pci_dev_admin; /**< The PCI device context */
	struct nvmf_doca_poll_group *doca_poll_group;  /**< Poll group where the QP should be created */
	uint64_t admin_cq_address;		       /**< The Host address of the CQ buffer */
	uint64_t admin_sq_address;		       /**< The Host address of the SQ buffer */
	uint16_t admin_cq_size;			       /**< The CQ size */
	uint16_t admin_sq_size;			       /**< The SQ size */
	struct nvmf_doca_admin_qp *admin_qp_out;       /**< The admin QP that was created */
};

/*
 * Message to indicate that create of the admin QP has finished
 *
 * This message is used as a response to the nvmf_doca_create_admin_qp() message
 * This message is sent to PCI device admin poll group once admin QP has been destroyed
 */
static void nvmf_doca_create_admin_qp_done(void *cb_arg)
{
	doca_error_t ret;
	struct nvmf_doca_create_admin_qp_ctx *ctx = cb_arg;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = ctx->pci_dev_admin;
	struct nvmf_doca_admin_qp *admin_qp = ctx->admin_qp_out;

	free(ctx);

	pci_dev_admin->state = NVMF_DOCA_LISTENER_INITIALIZED;
	char ready = 0x01;
	ret = doca_devemu_pci_dev_modify_bar_stateful_region_values(pci_dev_admin->pci_dev, 0, 28, &ready, 1);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to modify stateful region values %s", doca_error_get_name(ret));
		nvmf_doca_on_initialization_error(pci_dev_admin);
		return;
	}

	pci_dev_admin->admin_qp = admin_qp;
}

/*
 * Starts async flow of creating admin QP
 *
 * This message is sent from PCI device admin thread to admin QP thread
 * Once admin QP is created the admin QP thread will respond with nvmf_doca_create_admin_qp_done()
 */
static void nvmf_doca_create_admin_qp(void *cb_arg)
{
	struct nvmf_doca_create_admin_qp_ctx *ctx = cb_arg;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = ctx->pci_dev_admin;
	struct nvmf_doca_poll_group *doca_poll_group = ctx->doca_poll_group;
	struct spdk_thread *admin_thread = pci_dev_admin->doca_transport->admin_pg.thread;

	struct nvmf_doca_admin_qp *admin_qp = calloc(1, sizeof(*admin_qp));
	if (admin_qp == NULL) {
		DOCA_LOG_ERR("Failed to create admin QP: Out of memory");
		spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		return;
	}
	TAILQ_INIT(&admin_qp->io_cqs);
	TAILQ_INIT(&admin_qp->io_sqs);

	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg;
	doca_error_t ret = nvmf_doca_create_pci_dev_poll_group(pci_dev_admin, admin_qp, doca_poll_group, &pci_dev_pg);
	if (ret != DOCA_SUCCESS) {
		spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		return;
	}
	TAILQ_INSERT_TAIL(&doca_poll_group->pci_dev_pg_list, pci_dev_pg, link);

	struct nvmf_doca_io_create_attr io_attr = {
		.pe = doca_poll_group->admin_qp_pe,
		.dev = pci_dev_admin->emulation_manager->emulation_manager,
		.nvme_dev = pci_dev_admin->pci_dev,
		.dpa = pci_dev_admin->emulation_manager->dpa,
		.cq_id = NVMF_ADMIN_QUEUE_ID,
		.cq_depth = ctx->admin_cq_size,
		.host_cq_mmap = pci_dev_pg->host_mmap,
		.host_cq_address = ctx->admin_cq_address,
		.enable_msix = true,
		.msix_idx = 0,
		.max_num_sq = 1,
		.post_cqe_cb = nvmf_doca_on_post_cqe_complete,
		.fetch_sqe_cb = nvmf_doca_on_fetch_sqe_complete,
		.copy_data_cb = nvmf_doca_on_copy_data_complete,
		.stop_sq_cb = nvmf_doca_on_admin_sq_stop,
		.stop_io_cb = nvmf_doca_on_admin_cq_stop,
	};

	struct nvmf_doca_io *emulated_cq = calloc(1, sizeof(*emulated_cq));
	if (emulated_cq == NULL) {
		DOCA_LOG_ERR("Failed to create io: Failed to allocate IO struct");
		TAILQ_REMOVE(&doca_poll_group->pci_dev_pg_list, pci_dev_pg, link);
		nvmf_doca_destroy_pci_dev_poll_group(pci_dev_pg);
		spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		return;
	}
	ret = nvmf_doca_io_create(&io_attr, emulated_cq);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create io: %s", doca_error_get_name(ret));
		free(emulated_cq);
		TAILQ_REMOVE(&doca_poll_group->pci_dev_pg_list, pci_dev_pg, link);
		nvmf_doca_destroy_pci_dev_poll_group(pci_dev_pg);
		spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		return;
	}
	emulated_cq->poll_group = pci_dev_pg;
	emulated_cq->pci_dev_admin = pci_dev_admin;
	admin_qp->admin_cq = emulated_cq;

	struct nvmf_doca_sq *sq = calloc(1, sizeof(*sq));
	if (sq == NULL) {
		DOCA_LOG_ERR("Failed to create io: Failed to allocate SQ struct");
		nvmf_doca_io_destroy(emulated_cq);
		free(emulated_cq);
		TAILQ_REMOVE(&doca_poll_group->pci_dev_pg_list, pci_dev_pg, link);
		nvmf_doca_destroy_pci_dev_poll_group(pci_dev_pg);
		spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		return;
	}

	struct nvmf_doca_io_add_sq_attr sq_attr = {
		.pe = doca_poll_group->admin_qp_pe,
		.dev = pci_dev_admin->emulation_manager->emulation_manager,
		.nvme_dev = pci_dev_admin->pci_dev,
		.sq_depth = ctx->admin_sq_size,
		.host_sq_mmap = pci_dev_pg->host_mmap,
		.host_sq_address = ctx->admin_sq_address,
		.sq_id = NVMF_ADMIN_QUEUE_ID,
		.transport = doca_poll_group->pg.transport,
	};

	nvmf_doca_io_add_sq(emulated_cq, &sq_attr, sq);
	ctx->admin_qp_out = admin_qp;
	sq->ctx = ctx;
}

/*
 * Continues async flow of resetting the PCI device NVMf context
 *
 * @pci_dev_admin [in]: The PCI device admin context
 */
static void nvmf_doca_pci_dev_admin_reset_continue(struct nvmf_doca_pci_dev_admin *pci_dev_admin)
{
	doca_error_t result;

	/* Indicates that admin QP is destroyed we can now finalize the reset */
	if (pci_dev_admin->state != NVMF_DOCA_LISTENER_UNINITIALIZED) {
		pci_dev_admin->state = NVMF_DOCA_LISTENER_UNINITIALIZED;

		struct nvmf_doca_nvme_registers *registers = pci_dev_admin->stateful_region_values;
		if (registers->cc.bits.shn == SPDK_NVME_SHN_NORMAL || registers->cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			registers->csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
		}
		if (registers->cc.bits.en == 0) {
			registers->csts.bits.rdy = 0;
		}
		result = doca_devemu_pci_dev_modify_bar_stateful_region_values(pci_dev_admin->pci_dev,
									       0,
									       offsetof(struct spdk_nvme_registers,
											csts),
									       &registers->csts,
									       1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to complete controller reset/shutdown: Failed to modify stateful region - %s",
				doca_error_get_name(result));
			return;
		}
	}

	if (pci_dev_admin->is_flr) {
		doca_ctx_stop(doca_devemu_pci_dev_as_ctx(pci_dev_admin->pci_dev));
		pci_dev_admin->is_flr = false;
	}
}

/*
 * Starts async flow of resetting the PCI device NVMf context
 *
 * @pci_dev_admin [in]: The PCI device admin context
 */
static void nvmf_doca_pci_dev_admin_reset(struct nvmf_doca_pci_dev_admin *pci_dev_admin)
{
	if (pci_dev_admin->state != NVMF_DOCA_LISTENER_INITIALIZED &&
	    pci_dev_admin->state != NVMF_DOCA_LISTENER_INITIALIZATION_ERROR) {
		return;
	}
	pci_dev_admin->state = NVMF_DOCA_LISTENER_RESETTING;

	/* In case admin QP exist send message to destroy it */
	if (pci_dev_admin->admin_qp != NULL) {
		spdk_thread_exec_msg(pci_dev_admin->admin_qp_pg->pg.group->thread,
				     nvmf_doca_destroy_admin_qp,
				     pci_dev_admin);
		return;
	}

	nvmf_doca_pci_dev_admin_reset_continue(pci_dev_admin);
}

/*
 * FLR event handler callback
 *
 * @pci_dev [in]: PCI device
 * @user_data [in]: Data user
 */
static void flr_event_handler_cb(struct doca_devemu_pci_dev *pci_dev, union doca_data user_data)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	(void)pci_dev;

	struct nvmf_doca_pci_dev_admin *pci_dev_admin = user_data.ptr;

	pci_dev_admin->is_flr = true;

	nvmf_doca_pci_dev_admin_reset(pci_dev_admin);
}

/*
 * Hotplugstate change handler
 *
 * @pci_dev [in]: PCI device
 * @user_data [in]: Data user
 */
static void hotplug_state_change_handler_cb(struct doca_devemu_pci_dev *pci_dev, union doca_data user_data)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	(void)user_data;

	enum doca_devemu_pci_hotplug_state hotplug_state;
	doca_error_t ret;

	DOCA_LOG_INFO("Emulated device's hotplug state has changed");
	ret = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &hotplug_state);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to get hotplug state: %s", doca_error_get_name(ret));
		return;
	}
	DOCA_LOG_INFO("Hotplug state changed to %s", hotplug_state_to_string(hotplug_state));
}

/*
 * Callback invoked once admin SQ has been stopped
 *
 * @sq [in]: The NVMf DOCA SQ that was stopped
 */
static void nvmf_doca_on_admin_sq_stop(struct nvmf_doca_sq *sq)
{
	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg = sq->io->poll_group;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = pci_dev_pg->pci_dev_admin;
	struct nvmf_doca_admin_qp *admin_qp = pci_dev_pg->admin_qp;

	nvmf_doca_io_rm_sq(sq);
	free(sq);
	admin_qp->admin_sq = NULL;

	nvmf_doca_destroy_admin_qp_continue(pci_dev_admin);
}

/*
 * Handles errors that occur during the initialization flow
 *
 * @cb_arg [in]: The PCI device context
 */
static void nvmf_doca_on_initialization_error(void *cb_arg)
{
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = cb_arg;

	pci_dev_admin->state = NVMF_DOCA_LISTENER_INITIALIZATION_ERROR;
	nvmf_doca_pci_dev_admin_reset(pci_dev_admin);
}

/*
 * Handle events initiated by Host by writing to the controller registers
 *
 * @pci_dev [in]: The PCI device
 * @config [in]: The configuration of the stateful region describing location of the controller registers
 */
static void handle_controller_register_events(struct doca_devemu_pci_dev *pci_dev,
					      const struct bar_region_config *config)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	union doca_data ctx_user_data;
	doca_error_t ret;

	ret = doca_ctx_get_user_data(doca_devemu_pci_dev_as_ctx(pci_dev), &ctx_user_data);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get the context user data: %s", doca_error_get_name(ret));
		return;
	}

	struct nvmf_doca_pci_dev_admin *pci_dev_admin = ctx_user_data.ptr;

	ret = doca_devemu_pci_dev_query_bar_stateful_region_values(pci_dev,
								   config->bar_id,
								   config->start_address,
								   pci_dev_admin->stateful_region_values,
								   config->size);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query values of stateful region: %s", doca_error_get_name(ret));
		return;
	}
	if (pci_dev_admin->state == NVMF_DOCA_LISTENER_RESETTING ||
	    pci_dev_admin->state == NVMF_DOCA_LISTENER_INITIALIZING)
		return;

	struct nvmf_doca_nvme_registers *registers =
		(struct nvmf_doca_nvme_registers *)pci_dev_admin->stateful_region_values;

	if (registers->cc.bits.en == 1 && pci_dev_admin->state == NVMF_DOCA_LISTENER_UNINITIALIZED) {
		DOCA_LOG_INFO("Creating controller");

		pci_dev_admin->state = NVMF_DOCA_LISTENER_INITIALIZING;

		struct nvmf_doca_transport *doca_transport = pci_dev_admin->doca_transport;

		struct nvmf_doca_create_admin_qp_ctx *ctx = calloc(1, sizeof(*ctx));
		if (ctx == NULL) {
			DOCA_LOG_ERR("Failed to create admin QP: Out of memory");
			nvmf_doca_on_initialization_error(pci_dev_admin);
			return;
		}

		/* Choose any poll group to manage the admin QP */
		struct nvmf_doca_poll_group *doca_poll_group = choose_poll_group(doca_transport);
		*ctx = (struct nvmf_doca_create_admin_qp_ctx){
			.pci_dev_admin = pci_dev_admin,
			.doca_poll_group = doca_poll_group,
			.admin_cq_address = registers->acq,
			.admin_sq_address = registers->asq,
			.admin_cq_size = registers->aqa.bits.acqs + 1,
			.admin_sq_size = registers->aqa.bits.asqs + 1,
		};
		pci_dev_admin->admin_qp_pg = doca_poll_group;

		spdk_thread_exec_msg(doca_poll_group->pg.group->thread, nvmf_doca_create_admin_qp, ctx);
		return;
	}

	if (pci_dev_admin->state == NVMF_DOCA_LISTENER_INITIALIZED ||
	    pci_dev_admin->state == NVMF_DOCA_LISTENER_INITIALIZATION_ERROR) {
		if (registers->cc.bits.shn == SPDK_NVME_SHN_NORMAL || registers->cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			DOCA_LOG_INFO("Shut down controller");
			nvmf_doca_pci_dev_admin_reset(pci_dev_admin);
			return;
		}

		if (registers->cc.bits.en == 0) {
			DOCA_LOG_INFO("Resetting controller");
			nvmf_doca_pci_dev_admin_reset(pci_dev_admin);
			return;
		}
	}
}

/*
 * Stateful region write event handler
 *
 * @event [in]: stateful region write event
 * @user_data [in]: Data user
 */
static void stateful_region_write_event_handler_cb(
	struct doca_devemu_pci_dev_event_bar_stateful_region_driver_write *event,
	union doca_data user_data)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct doca_devemu_pci_dev *pci_dev =
		doca_devemu_pci_dev_event_bar_stateful_region_driver_write_get_pci_dev(event);
	const struct bar_region_config *config = (const struct bar_region_config *)user_data.ptr;

	handle_controller_register_events(pci_dev, config);
}

/*
 * Register to the stateful region write event of the emulated device for all stateful regions of configured type
 *
 * @pci_dev [in]: The emulated device context
 * @pci_dev_admin [in]: NVMF device context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_to_stateful_region_write_events(struct doca_devemu_pci_dev *pci_dev,
							     struct nvmf_doca_pci_dev_admin *pci_dev_admin)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	const struct bar_region_config *config;
	uint64_t region_idx;
	union doca_data user_data;
	doca_error_t ret;
	uint64_t max_region_size = 0;

	for (region_idx = 0; region_idx < PCI_TYPE_NUM_BAR_STATEFUL_REGIONS; region_idx++) {
		config = &stateful_configs[region_idx];
		user_data.ptr = (void *)config;
		ret = doca_devemu_pci_dev_event_bar_stateful_region_driver_write_register(
			pci_dev,
			stateful_region_write_event_handler_cb,
			config->bar_id,
			config->start_address,
			user_data);

		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Unable to register to emulated PCI device stateful region write event: %s",
				     doca_error_get_name(ret));
			return ret;
		}

		max_region_size = max_region_size > config->size ? max_region_size : config->size;
	}

	user_data.ptr = (void *)pci_dev_admin;
	ret = doca_ctx_set_user_data(doca_devemu_pci_dev_as_ctx(pci_dev), user_data);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set context user data: %s", doca_error_get_name(ret));
		return ret;
	}

	/* Setup a buffer that can be used to query stateful region values once event is triggered */
	pci_dev_admin->stateful_region_values = calloc(1, max_region_size);
	if (pci_dev_admin->stateful_region_values == NULL) {
		DOCA_LOG_ERR("Unable to allocate buffer for storing stateful region values: out of memory");
		return DOCA_ERROR_NO_MEMORY;
	}

	return DOCA_SUCCESS;
}

/*
 * Registers handlers and starts context
 *
 * @doca_emulation_manager [in]: Emulation manager
 * @pci_dev_admin [in]: PCI device admin context
 * @return: DOCA_SUCCESS on success, and other error code on failure
 */
static doca_error_t register_handlers_set_datapath_and_start(struct nvmf_doca_emulation_manager *doca_emulation_manager,
							     struct nvmf_doca_pci_dev_admin *pci_dev_admin)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	union doca_data user_data;
	doca_error_t ret;
	user_data.ptr = (void *)pci_dev_admin;

	ret = doca_ctx_set_datapath_on_dpa(doca_devemu_pci_dev_as_ctx(pci_dev_admin->pci_dev),
					   doca_emulation_manager->dpa);

	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set PCI emulated device context datapath on DPA: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = doca_ctx_set_user_data(doca_devemu_pci_dev_as_ctx(pci_dev_admin->pci_dev), user_data);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set user data: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = doca_ctx_set_state_changed_cb(doca_devemu_pci_dev_as_ctx(pci_dev_admin->pci_dev),
					    devemu_state_changed_cb);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to set state change callback: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = doca_devemu_pci_dev_event_hotplug_state_change_register(pci_dev_admin->pci_dev,
								      hotplug_state_change_handler_cb,
								      user_data);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register to hotplug state change callback: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = doca_devemu_pci_dev_event_flr_register(pci_dev_admin->pci_dev, flr_event_handler_cb, user_data);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register to FLR event: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = register_to_stateful_region_write_events(pci_dev_admin->pci_dev, pci_dev_admin);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Unable to register to emulated PCI device stateful region write event: %s",
			     doca_error_get_name(ret));
		return ret;
	}

	ret = doca_ctx_start(doca_devemu_pci_dev_as_ctx(pci_dev_admin->pci_dev));
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context: %s", doca_error_get_name(ret));
		return ret;
	}
	return DOCA_SUCCESS;
}

/*
 * Creates and starts mmap
 *
 * @pci_dev [in]: PCI device
 * @emulation_manager [in]: Emulation manager
 * @mmap_out [out]: The created mmap
 * @return: DOCA_SUCCESS on success, and an error code on failure
 */
static doca_error_t nvmf_doca_create_host_mmap(struct doca_devemu_pci_dev *pci_dev,
					       struct doca_dev *emulation_manager,
					       struct doca_mmap **mmap_out)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct doca_mmap *mmap;
	doca_error_t ret;

	ret = doca_devemu_pci_mmap_create(pci_dev, &mmap);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap for pci device emulation: %s", doca_error_get_name(ret));
		return ret;
	}

	ret = doca_mmap_set_max_num_devices(mmap, 1);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set mmap max number of devices: %s", doca_error_get_name(ret));
		goto destroy_mmap;
	}

	ret = doca_mmap_add_dev(mmap, emulation_manager);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device to mmap: %s", doca_error_get_name(ret));
		goto destroy_mmap;
	}

	ret = doca_mmap_set_permissions(mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memory permissions: %s", doca_error_get_name(ret));
		goto destroy_mmap;
	}

	ret = doca_mmap_set_memrange(mmap, 0, UINT64_MAX);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memrange for mmap: %s", doca_error_get_name(ret));
		goto destroy_mmap;
	}

	ret = doca_mmap_start(mmap);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start mmap: %s", doca_error_get_name(ret));
		goto destroy_mmap;
	}

	*mmap_out = mmap;

	return DOCA_SUCCESS;

destroy_mmap:
	doca_mmap_destroy(mmap);
	return ret;
}

static void nvmf_doca_pci_dev_admin_destroy(struct nvmf_doca_pci_dev_admin *pci_dev_admin)
{
	doca_error_t ret;

	if (pci_dev_admin->pci_dev != NULL) {
		pci_dev_admin->is_destroy_flow = true;
		ret = doca_ctx_stop(doca_devemu_pci_dev_as_ctx(pci_dev_admin->pci_dev));
		if (ret != DOCA_SUCCESS && ret != DOCA_ERROR_BAD_STATE) {
			DOCA_LOG_ERR("Failed to stop DOCA Emulated Device context: %s", doca_error_get_name(ret));
		}

		ret = doca_devemu_pci_dev_destroy(pci_dev_admin->pci_dev);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA Emulated Device context: %s", doca_error_get_name(ret));
		}
		pci_dev_admin->pci_dev = NULL;
	}

	if (pci_dev_admin->stateful_region_values) {
		free(pci_dev_admin->stateful_region_values);
		pci_dev_admin->stateful_region_values = NULL;
	}

	if (pci_dev_admin->dev_rep != NULL) {
		ret = doca_dev_rep_close(pci_dev_admin->dev_rep);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy PCI device context: Failed to close representor - %s",
				     doca_error_get_name(ret));
		}
	}

	free(pci_dev_admin);
}

static int nvmf_doca_pci_dev_admin_create(struct nvmf_doca_transport *doca_transport,
					  const struct spdk_nvme_transport_id *trid,
					  struct nvmf_doca_pci_dev_admin **pci_dev_admin_out)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_emulation_manager *doca_emulation_manager;
	struct doca_dev_rep *dev_rep;
	doca_error_t ret;
	int err = 0;

	ret = find_emulation_manager_and_function_by_vuid(doca_transport,
							  (char *)trid->traddr,
							  &doca_emulation_manager,
							  &dev_rep);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Could not find an emulation manager and a function with the given address: %s",
			     doca_error_get_name(ret));
		return -ENXIO;
	}

	struct nvmf_doca_pci_dev_admin *pci_dev_admin = calloc(1, sizeof(*pci_dev_admin));
	if (pci_dev_admin == NULL) {
		DOCA_LOG_ERR("Failed to allocate PCI device context's memory: errno %d", err);
		doca_dev_rep_close(dev_rep);
		return -ENOMEM;
	}
	pci_dev_admin->dev_rep = dev_rep;
	pci_dev_admin->subsystem = NULL;
	pci_dev_admin->doca_transport = doca_transport;
	pci_dev_admin->emulation_manager = doca_emulation_manager;
	pci_dev_admin->state = NVMF_DOCA_LISTENER_UNINITIALIZED;
	pci_dev_admin->ctlr_id = 0;
	memcpy(&pci_dev_admin->trid, trid, sizeof(pci_dev_admin->trid));

	/* Assign PCI device to admin poll group the poll group will be responsible for managing this device */
	struct nvmf_doca_admin_poll_group *admin_pg = &doca_transport->admin_pg;
	ret = doca_devemu_pci_dev_create(doca_emulation_manager->pci_type,
					 dev_rep,
					 admin_pg->pe,
					 &pci_dev_admin->pci_dev);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create PCI device: %s", doca_error_get_name(ret));
		nvmf_doca_pci_dev_admin_destroy(pci_dev_admin);
		return -EINVAL;
	}

	ret = register_handlers_set_datapath_and_start(doca_emulation_manager, pci_dev_admin);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Faield to register handler and start context: %s", doca_error_get_name(ret));
		nvmf_doca_pci_dev_admin_destroy(pci_dev_admin);
		return -EINVAL;
	}

	*pci_dev_admin_out = pci_dev_admin;

	return EXIT_SUCCESS;
}

static doca_error_t devemu_hotplug_transition_wait(struct nvmf_doca_pci_dev_admin *pci_dev_admin,
						   enum doca_devemu_pci_hotplug_state new_state,
						   size_t timeout_in_micros)
{
	static const size_t sleep_in_micros = 10;
	static const size_t sleep_in_nanos = sleep_in_micros * 1000;

	struct timespec timespec = {
		.tv_sec = 0,
		.tv_nsec = sleep_in_nanos,
	};
	doca_error_t ret;
	enum doca_devemu_pci_hotplug_state current_state;

	size_t elapsed_time_in_micros = 0;
	do {
		if (elapsed_time_in_micros >= timeout_in_micros) {
			DOCA_LOG_ERR("Failed to wait for hotplug state to change: Timed out");
			return DOCA_ERROR_TIME_OUT;
		}
		if (doca_pe_progress(pci_dev_admin->doca_transport->admin_pg.pe) == 0)
			nanosleep(&timespec, NULL);
		elapsed_time_in_micros += sleep_in_micros;
		ret = doca_devemu_pci_dev_get_hotplug_state(pci_dev_admin->pci_dev, &current_state);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to wait for hotplug state to change: Failed to get hotplug state %s",
				     doca_error_get_name(ret));
			return ret;
		}
	} while (current_state != new_state);

	return DOCA_SUCCESS;
}

/*
 * Adds a listener to the DOCA transport at the given address
 *
 * Callback invoked by the NVMf target once user issues the add listener RPC
 * The callback will hotplug the emulated device towards the Host, and start listening on interactions from Host
 *
 * @transport [in]: The DOCA transport
 * @trid [in]: The transport ID containing the address to listen on, in this case the VUID of the emulated device
 * @listen_opts [in]: The listen options
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_listen(struct spdk_nvmf_transport *transport,
			    const struct spdk_nvme_transport_id *trid,
			    struct spdk_nvmf_listen_opts *listen_opts)
{
	(void)listen_opts;

	doca_error_t ret;
	int err = 0;

	struct nvmf_doca_transport *doca_transport = SPDK_CONTAINEROF(transport, struct nvmf_doca_transport, transport);
	ret = check_for_duplicate(doca_transport, trid->traddr);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Emulated device already is listened to by this transport: %s", doca_error_get_name(ret));
		err = -EEXIST;
		goto exit;
	}

	struct nvmf_doca_pci_dev_admin *pci_dev_admin;
	err = nvmf_doca_pci_dev_admin_create(doca_transport, trid, &pci_dev_admin);
	if (err != 0) {
		goto exit;
	}
	TAILQ_INSERT_TAIL(&doca_transport->admin_pg.pci_dev_admins, pci_dev_admin, link);

	ret = doca_devemu_pci_dev_hotplug(pci_dev_admin->pci_dev);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to hotplug pci device: %s", doca_error_get_name(ret));
		err = -EINVAL;
		if (ret == DOCA_ERROR_AGAIN) {
			err = -EAGAIN;
		}
		goto destroy_pci_dev_admin;
	}
	DOCA_LOG_INFO("Hotplug initiated waiting for host to notice new device");

	ret = devemu_hotplug_transition_wait(pci_dev_admin,
					     DOCA_DEVEMU_PCI_HP_STATE_POWER_ON,
					     HOTPLUG_TIMEOUT_IN_MICROS);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start listen: Could not transition device to POWER_ON state - %s",
			     doca_error_get_name(ret));
		err = -EINVAL;
		goto unplug;
	}
	doca_transport->num_of_listeners++;
	return EXIT_SUCCESS;

unplug:
	doca_devemu_pci_dev_hotunplug(pci_dev_admin->pci_dev);
destroy_pci_dev_admin:
	TAILQ_REMOVE(&doca_transport->admin_pg.pci_dev_admins, pci_dev_admin, link);
	nvmf_doca_pci_dev_admin_destroy(pci_dev_admin);
exit:
	return err;
}

/*
 * Removes a listener from the DOCA transport at the given address
 *
 * Callback invoked by the NVMf target once user issues the remove listener RPC
 * The callback will hotunplug the emulated device from the Host, preventing further interactions
 *
 * @transport [in]: The DOCA transport
 * @trid [in]: The transport ID containing the address to stop listen on, in this case the VUID of the emulated device
 */
static void nvmf_doca_stop_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_transport *doca_transport = SPDK_CONTAINEROF(transport, struct nvmf_doca_transport, transport);
	doca_error_t ret;

	struct nvmf_doca_pci_dev_admin *pci_dev_admin =
		nvmf_doca_transport_find_pci_dev_admin(doca_transport, trid->traddr);
	if (pci_dev_admin == NULL) {
		DOCA_LOG_ERR("Failed to stop listen: Could not find a PCI device (listener) with the requested VUID");
		return;
	}

	/* If the admin QP exists, executing a stop_listen may not function correctly due to the resources associated
	 * with the admin QP that still need to be managed */
	if (pci_dev_admin->admin_qp != NULL) {
		DOCA_LOG_ERR("The QP and its resources must be freed before stopping the listeners %s", __func__);
		return;
	}

	ret = doca_devemu_pci_dev_hotunplug(pci_dev_admin->pci_dev);
	if (ret == DOCA_SUCCESS) {
		DOCA_LOG_INFO("Hotplug initiated waiting for host to notice new device");
		ret = devemu_hotplug_transition_wait(pci_dev_admin,
						     DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF,
						     HOTPLUG_TIMEOUT_IN_MICROS);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to stop listen: Could not transition device to POWER_OFF state - %s",
				     doca_error_get_name(ret));
		}
	} else {
		DOCA_LOG_ERR("Failed to hotunplug pci device: %s", doca_error_get_name(ret));
	}

	TAILQ_REMOVE(&doca_transport->admin_pg.pci_dev_admins, pci_dev_admin, link);
	nvmf_doca_pci_dev_admin_destroy(pci_dev_admin);
}

/*
 * Associates a listener with the given address with an NVMf subsystem
 *
 * Callback invoked by the NVMf target once user issues the add listener RPC
 *
 * @transport [in]: The DOCA transport
 * @subsystem [in]: The NVMf subsystem
 * @trid [in]: The transport ID containing the address of the listener, in this case the VUID of the emulated device
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_listen_associate(struct spdk_nvmf_transport *transport,
				      const struct spdk_nvmf_subsystem *subsystem,
				      const struct spdk_nvme_transport_id *trid)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_transport *doca_transport = SPDK_CONTAINEROF(transport, struct nvmf_doca_transport, transport);

	struct nvmf_doca_pci_dev_admin *pci_dev_admin =
		nvmf_doca_transport_find_pci_dev_admin(doca_transport, trid->traddr);
	if (pci_dev_admin == NULL) {
		return -ENXIO;
	}

	pci_dev_admin->subsystem = (struct spdk_nvmf_subsystem *)subsystem;

	return EXIT_SUCCESS;
}

/*
 * Creates a poll group for polling the DOCA transport
 *
 * Callback invoked by the NVMf target on each thread after the transport has been created
 *
 * @transport [in]: The DOCA transport
 * @group [in]: The NVMf target poll group
 * @return: The newly created DOCA transport poll group on success and NULL otherwise
 */
static struct spdk_nvmf_transport_poll_group *nvmf_doca_poll_group_create(struct spdk_nvmf_transport *transport,
									  struct spdk_nvmf_poll_group *group)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	(void)group;

	doca_error_t ret;
	struct nvmf_doca_poll_group *doca_pg;
	struct nvmf_doca_transport *doca_transport = SPDK_CONTAINEROF(transport, struct nvmf_doca_transport, transport);

	doca_pg = (struct nvmf_doca_poll_group *)calloc(1, sizeof(struct nvmf_doca_poll_group));
	if (doca_pg == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for doca poll group");
		return NULL;
	}

	ret = doca_pe_create(&doca_pg->pe);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine %s", doca_error_get_name(ret));
		free(doca_pg);
		return NULL;
	}

	ret = doca_pe_create(&doca_pg->admin_qp_pe);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine %s", doca_error_get_name(ret));
		doca_pe_destroy(doca_pg->pe);
		free(doca_pg);
		return NULL;
	}

	doca_pg->admin_qp_poll_rate_limiter = 0;

	TAILQ_INIT(&doca_pg->pci_dev_pg_list);

	TAILQ_INSERT_TAIL(&doca_transport->poll_groups, doca_pg, link);

	return &doca_pg->pg;
}

/*
 * Destroy the DOCA transport poll group
 *
 * Callback invoked by the NVMf target before attempting to destroy the DOCA transport
 *
 * @group [in]: The poll group to destroy
 */
static void nvmf_doca_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_poll_group *doca_pg;
	struct nvmf_doca_transport *doca_transport;

	doca_pg = SPDK_CONTAINEROF(group, struct nvmf_doca_poll_group, pg);
	doca_transport = SPDK_CONTAINEROF(group->transport, struct nvmf_doca_transport, transport);

	doca_pe_destroy(doca_pg->admin_qp_pe);
	doca_pe_destroy(doca_pg->pe);
	TAILQ_REMOVE(&doca_transport->poll_groups, doca_pg, link);
	free(doca_pg);
}

/*
 * Picks the optimal poll group to add the QP to
 *
 * Callback invoked by the NVMf target after creation of qpair is complete but before calling nvmf_doca_poll_group_add
 *
 * @qpair [in]: The newly created NVMf QPair
 * @return: An existing DOCA transport poll group
 */
static struct spdk_nvmf_transport_poll_group *nvmf_doca_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_sq *sq = SPDK_CONTAINEROF(qpair, struct nvmf_doca_sq, spdk_qp);

	/* Ensure that CQ along with SQs attached to it will run on same poll group */
	return &sq->io->poll_group->poll_group->pg;
}

/*
 * Set the controller status to ready.
 *
 * Callback invoked after nvmf_doca_set_property completes
 *
 * @request [in]: The request that triggered this callback
 * @cb_arg [in]: The call back argument which is the SQ
 */
static void enable_nvmf_controller_cb(struct nvmf_doca_request *request, void *cb_arg)
{
	(void)request;

	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_sq *sq = cb_arg;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = sq->io->poll_group->pci_dev_admin;
	struct spdk_thread *admin_thread = pci_dev_admin->doca_transport->admin_pg.thread;

	void *create_admin_qp_ctx = sq->ctx;
	sq->ctx = NULL;
	spdk_thread_exec_msg(admin_thread, nvmf_doca_create_admin_qp_done, create_admin_qp_ctx);
}

/*
 * Sets properties on the NVMF subsystem.
 *
 * Callback invoked from nvmf_doca_connect_spdk_qp_done
 *
 * @doca_sq [in]: The admin SQ that will execute the command
 */
static void nvmf_doca_set_property(struct nvmf_doca_sq *doca_sq)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	struct nvmf_doca_request *new_request;
	doca_error_t ret;

	struct nvmf_doca_pci_dev_admin *pci_dev_admin = doca_sq->io->poll_group->pci_dev_admin;
	struct spdk_thread *admin_thread = pci_dev_admin->doca_transport->admin_pg.thread;

	new_request = nvmf_doca_request_get(doca_sq);
	if (new_request == NULL) {
		spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		return;
	}

	struct spdk_nvme_registers *registers = pci_dev_admin->stateful_region_values;

	new_request->request.cmd->prop_set_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	new_request->request.cmd->prop_set_cmd.cid = 0;
	new_request->request.cmd->prop_set_cmd.attrib.size = 0;
	new_request->request.cmd->prop_set_cmd.ofst = offsetof(struct spdk_nvme_registers, cc);
	new_request->request.cmd->prop_set_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET;
	new_request->request.length = 4;

	ret = doca_devemu_pci_dev_query_bar_stateful_region_values(pci_dev_admin->pci_dev,
								   0,
								   offsetof(struct spdk_nvme_registers, cc),
								   &registers->cc,
								   4);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query stateful region values %s", doca_error_get_name(ret));
		spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		return;
	}

	int temp_iovcnt = (int)new_request->request.iovcnt;

	new_request->request.cmd->prop_set_cmd.value.u32.low = registers->cc.raw;
	spdk_iov_one(new_request->request.iov, &temp_iovcnt, &registers->cc, new_request->request.length);

	new_request->doca_cb = enable_nvmf_controller_cb;
	new_request->cb_arg = doca_sq;

	spdk_nvmf_request_exec_fabrics(&new_request->request);
}

struct nvmf_doca_poll_group_create_io_sq_ctx {
	struct nvmf_doca_request *request;		 /**< The original create IO SQ admin command */
	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg; /**< Poll group where SQ should be created */
	struct nvmf_doca_io *io_cq;			 /**< The IO CQ to connect to the SQ */
	struct nvmf_doca_sq *io_sq;			 /**< The IO SQ to initialize */
};

/*
 * Method to be called once async 'nvmf_doca_poll_group_create_io_sq()' completes
 *
 * @args [in]: The context of the async procedure
 */
static void nvmf_doca_poll_group_create_io_sq_done(void *args);

/*
 * Method to be called once async 'nvmf_doca_connect_spdk_qp()' completes
 *
 * @request [in]: The connect request that completed
 * @cb_arg [in]: The argument passed along this callback
 */
static void nvmf_doca_connect_spdk_qp_done(struct nvmf_doca_request *request, void *cb_arg)
{
	struct nvmf_doca_sq *sq = cb_arg;
	struct nvmf_doca_request *doca_request;

	nvmf_doca_request_free(request);

	if (sq->sq_id == NVMF_ADMIN_QUEUE_ID) {
		nvmf_doca_set_property(sq);
		return;
	}

	struct nvmf_doca_poll_group_create_io_sq_ctx *ctx = sq->ctx;
	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg = ctx->pci_dev_pg;

	doca_request = ctx->request;
	doca_request->request.rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SUCCESS;
	doca_request->request.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	sq->ctx = NULL;

	struct spdk_thread *admin_qp_thread = pci_dev_pg->pci_dev_admin->admin_qp_pg->pg.group->thread;
	spdk_thread_exec_msg(admin_qp_thread, nvmf_doca_poll_group_create_io_sq_done, ctx);
}

/*
 * Connect an SPDK QP of an SQ
 *
 * This method is async once complete nvmf_doca_connect_spdk_qp_done will be called
 *
 * @sq [in]: The SQ containing the SPDK QP to connect
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_connect_spdk_qp(struct nvmf_doca_sq *sq)
{
	struct nvmf_doca_request *doca_request;
	struct spdk_nvmf_fabric_connect_data *data;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = sq->io->poll_group->pci_dev_admin;
	struct spdk_thread *admin_thread = pci_dev_admin->doca_transport->admin_pg.thread;

	doca_request = nvmf_doca_request_get(sq);
	if (doca_request == NULL) {
		if (sq->sq_id == NVMF_ADMIN_QUEUE_ID) {
			spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		}
		return -ENOMEM;
	}

	doca_request->request.cmd->connect_cmd.opcode = SPDK_NVME_OPC_FABRIC;
	doca_request->request.cmd->connect_cmd.cid = 0;
	doca_request->request.cmd->connect_cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	doca_request->request.cmd->connect_cmd.recfmt = 0;
	doca_request->request.cmd->connect_cmd.sqsize = sq->queue.num_elements - 1;
	doca_request->request.cmd->connect_cmd.qid = sq->sq_id;
	doca_request->request.length = sizeof(struct spdk_nvmf_fabric_connect_data);

	data = calloc(1, doca_request->request.length);
	if (data == NULL) {
		if (sq->sq_id == NVMF_ADMIN_QUEUE_ID) {
			spdk_thread_exec_msg(admin_thread, nvmf_doca_on_initialization_error, pci_dev_admin);
		}
		return -ENOMEM;
	}
	doca_request->data_from_alloc = true;

	if (pci_dev_admin->ctlr_id == 0) {
		data->cntlid = pci_dev_admin->doca_transport->num_of_listeners;
		pci_dev_admin->ctlr_id = pci_dev_admin->doca_transport->num_of_listeners;
	} else {
		data->cntlid = pci_dev_admin->ctlr_id;
	}

	snprintf((char *)data->subnqn,
		 sizeof(data->subnqn),
		 "%s",
		 spdk_nvmf_subsystem_get_nqn(pci_dev_admin->subsystem));

	doca_request->doca_cb = nvmf_doca_connect_spdk_qp_done;
	doca_request->cb_arg = sq;

	doca_request->request.data = data;
	spdk_nvmf_request_exec_fabrics(&doca_request->request);

	return 0;
}

/*
 * Assigns QP to a poll group such that it will be responsible for polling the QP
 *
 * Callback invoked by the NVMf target after creation of qpair
 * Once this callback is invoked then it means both the NVMf DOCA SQ and SPDK NVMf QPair have been created
 *
 * @group [in]: The DOCA transport poll group as picked by nvmf_doca_get_optimal_poll_group
 * @qpair [in]: The newly created NVMf QPair
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_poll_group_add(struct spdk_nvmf_transport_poll_group *group, struct spdk_nvmf_qpair *qpair)
{
	DOCA_LOG_DBG("Entering function %s", __func__);

	(void)group;

	struct nvmf_doca_sq *doca_sq;

	doca_sq = SPDK_CONTAINEROF(qpair, struct nvmf_doca_sq, spdk_qp);
	if (doca_sq->sq_id == NVMF_ADMIN_QUEUE_ID) {
		doca_sq->io->poll_group->admin_qp->admin_sq = doca_sq;
	}

	return nvmf_doca_connect_spdk_qp(doca_sq);
}

/*
 * Removes QP from poll group
 *
 * Callback invoked by the NVMf target on destroy of qpair
 *
 * @group [in]: The DOCA transport poll group the QP is assigned to
 * @qpair [in]: The NVMf QPair to be removed
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_poll_group_remove(struct spdk_nvmf_transport_poll_group *group, struct spdk_nvmf_qpair *qpair)
{
	DOCA_LOG_DBG("Entering function: %s", __func__);

	(void)group;
	(void)qpair;

	return 0;
}

/*
 * Polls the DOCA transport poll group
 *
 * Callback invoked by reactor thread frequently
 *
 * @group [in]: The DOCA transport poll group as picked by nvmf_doca_get_optimal_poll_group
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct nvmf_doca_poll_group *doca_pg = SPDK_CONTAINEROF(group, struct nvmf_doca_poll_group, pg);

	doca_pe_progress(doca_pg->pe);

	/* Polling for the admin QP typically involves lighter workloads compared to I/O QPs, which are more active
	and handle a greater number of tasks. By reducing the polling rate for the admin QP to once for every
	ADMIN_QP_POLL_RATE_LIMIT (1000) I/O QP polls, performance can be enhanced.
	However, this method may accidentally slow down the device destruction process. Each inflight task needs
	separate access to the progress engine to be properly released. When there are numerous inflight tasks across
	multiple devices on the same thread, infrequent polling of the admin QP leads to a slower overall cleanup
	process. */

	if (doca_pg->admin_qp_poll_rate_limiter % ADMIN_QP_POLL_RATE_LIMIT == 0) {
		doca_pe_progress(doca_pg->admin_qp_pe);
	}
	doca_pg->admin_qp_poll_rate_limiter++;

	return 0;
}

/*
 * Frees a completed NVMf request back to the pool
 *
 * Callback invoked by NVMf target once a request can be freed
 *
 * @req [in]: The NVMf request to free
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_req_free(struct spdk_nvmf_request *req)
{
	struct nvmf_doca_request *request = SPDK_CONTAINEROF(req, struct nvmf_doca_request, request);

	nvmf_doca_request_free(request);

	return 0;
}

/*
 * Completes the NVMf request
 *
 * Callback invoked by NVMf target once a request has been completed but before freeing it
 *
 * @req [in]: The NVMf request to complete
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_req_complete(struct spdk_nvmf_request *req)
{
	struct nvmf_doca_request *request = SPDK_CONTAINEROF(req, struct nvmf_doca_request, request);

	nvmf_doca_request_complete(request);

	return 0;
}

/*
 * Destroys the NVMf QP
 *
 * Callback invoked by the NVMf target once the QP can be destroyed
 *
 * @qpair [in]: The NVMf QPair
 * @cb_fn [in]: Callback to be invoked once close finishes - can be NULL
 * @cb_arg [in]: Argument to be passed to the callback
 */
static void nvmf_doca_close_qpair(struct spdk_nvmf_qpair *qpair, spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	DOCA_LOG_DBG("Entering function: %s", __func__);

	(void)qpair;

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

/*
 * Get the listener address from the QP
 *
 * @qpair [in]: The NVMf QPair
 * @trid [out]: The transport ID containing the address related to the QP, in this case the VUID of the emulated device
 * @return: 0 on success and negative error code otherwise
 */
static int nvmf_doca_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid)
{
	DOCA_LOG_DBG("Entering function: %s", __func__);

	struct nvmf_doca_sq *sq = SPDK_CONTAINEROF(qpair, struct nvmf_doca_sq, spdk_qp);

	struct nvmf_doca_pci_dev_admin *pci_dev_admin = sq->io->poll_group->pci_dev_admin;

	memcpy(trid, &pci_dev_admin->trid, sizeof(*trid));

	return 0;
}

/*********************************************************************************************************************
 * Data Path
 *********************************************************************************************************************/

#define IDENTIFY_CMD_DATA_BUFFER_SIZE 4096
#define FEAT_CMD_LBA_RANGE_SIZE 4096
#define FEAT_CMD_AUTONOMOUS_POWER_STATE_TRANSITION_SIZE 256
#define FEAT_CMD_TIMESTAMP_SIZE 8
#define FEAT_CMD_HOST_BEHAVIOR_SUPPORT_SIZE 512
#define FEAT_CMD_HOST_IDENTIFIER_EXT_SIZE 16
#define FEAT_CMD_HOST_IDENTIFIER_SIZE 8

/*
 * Map the data described by PRP list entries used in NVME command in IOV structres
 *
 * @request [in]: The NVMf request which holds the ??
 * @arg [in]: Argument associated with the callback
 */
static void copy_prp_list_data(struct nvmf_doca_request *request, void *arg)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	(void)arg;
	void *data_out_address;
	uintptr_t *prp_list_addr;
	uint32_t idx = 1; /* idx 0 is already occupied */
	uint32_t remaining_length;
	uint32_t length = request->residual_length;

	doca_buf_get_head(request->prp_dpu_buf, (void **)&prp_list_addr);

	doca_buf_dec_refcount(request->prp_dpu_buf, NULL);
	request->prp_dpu_buf = NULL;
	doca_buf_dec_refcount(request->prp_host_buf, NULL);
	request->prp_host_buf = NULL;

	/* Iterate over the prp entries */
	while (length != 0) {
		remaining_length = spdk_min(length, NVME_PAGE_SIZE);

		request->host_buffer[idx] = nvmf_doca_sq_get_host_buffer(request->doca_sq, prp_list_addr[idx - 1]);
		request->dpu_buffer[idx] = nvmf_doca_sq_get_dpu_buffer(request->doca_sq);
		doca_buf_get_head(request->dpu_buffer[idx], &data_out_address);

		request->request.iov[idx].iov_base = data_out_address;
		request->request.iov[idx].iov_len = remaining_length;
		request->request.iovcnt++;

		length -= remaining_length;
		idx++;
	}
	request->num_of_buffers = request->request.iovcnt;

	if (request->request.cmd->nvme_cmd.opc == SPDK_NVME_OPC_WRITE) {
		buffers_ready_copy_data_host_to_dpu(request);
	} else {
		buffers_ready_copy_data_dpu_to_host(request);
	}
}

/*
 * This method is responsible for mapping the data described by PRP entries used in NVME command in IOV structres.
 *
 * @request [in]: The NVMf request
 */
static void nvme_cmd_map_prps(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	uint64_t prp1, prp2;
	uint32_t remaining_length, number_of_pages;
	uint64_t *prp_list;
	uint32_t length = request->request.length;

	prp1 = request->request.cmd->nvme_cmd.dptr.prp.prp1;
	prp2 = request->request.cmd->nvme_cmd.dptr.prp.prp2;

	/* PRP1 may start with unaligned page address */
	remaining_length = NVME_PAGE_SIZE - (prp1 % NVME_PAGE_SIZE);
	remaining_length = spdk_min(length, remaining_length);

	void *data_out_address;
	request->host_buffer[0] = nvmf_doca_sq_get_host_buffer(request->doca_sq, prp1);
	request->dpu_buffer[0] = nvmf_doca_sq_get_dpu_buffer(request->doca_sq);
	doca_buf_get_head(request->dpu_buffer[0], &data_out_address);

	request->request.iov[0].iov_base = data_out_address;
	request->request.iov[0].iov_len = remaining_length;
	request->request.iovcnt++;

	length -= remaining_length;

	if (length == 0) {
		/* There is only one prp entry */
		request->num_of_buffers = request->request.iovcnt;

		if (request->request.cmd->nvme_cmd.opc == SPDK_NVME_OPC_WRITE) {
			buffers_ready_copy_data_host_to_dpu(request);
		} else {
			buffers_ready_copy_data_dpu_to_host(request);
		}

	} else if (length <= NVME_PAGE_SIZE) {
		/* Data crosses exactly one memory page boundray, there are two PRP entries */
		request->host_buffer[1] = nvmf_doca_sq_get_host_buffer(request->doca_sq, prp2);
		request->dpu_buffer[1] = nvmf_doca_sq_get_dpu_buffer(request->doca_sq);
		doca_buf_get_head(request->dpu_buffer[1], &data_out_address);

		request->request.iov[1].iov_base = data_out_address;
		request->request.iov[1].iov_len = length;
		request->request.iovcnt++;
		request->num_of_buffers = request->request.iovcnt;

		if (request->request.cmd->nvme_cmd.opc == SPDK_NVME_OPC_WRITE) {
			buffers_ready_copy_data_host_to_dpu(request);
		} else {
			buffers_ready_copy_data_dpu_to_host(request);
		}
	} else {
		/* PRP list used and prp2 holds a pointer to it*/
		number_of_pages = SPDK_CEIL_DIV(length, NVME_PAGE_SIZE);

		request->prp_host_buf = nvmf_doca_sq_get_host_buffer(request->doca_sq, prp2);
		request->prp_dpu_buf = nvmf_doca_sq_get_dpu_buffer(request->doca_sq);

		union doca_data user_data;
		user_data.ptr = request;
		request->residual_length = length;
		request->doca_cb = copy_prp_list_data;
		request->num_of_buffers = 1;

		nvmf_doca_sq_copy_data(request->doca_sq,
				       request->prp_dpu_buf,
				       request->prp_host_buf,
				       number_of_pages * sizeof(*prp_list),
				       user_data);
	}
}

/*
 * Initialize Host and DPU data buffers for data transfer
 *
 * @request [in]: The NVMf request
 */
static void init_dpu_host_buffers(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	if (request->doca_sq->sq_id != NVMF_ADMIN_QUEUE_ID) {
		if (request->request.cmd->nvme_cmd.psdt == SPDK_NVME_PSDT_PRP) {
			return nvme_cmd_map_prps(request);
		}
	}

	void *data_out_address;
	uintptr_t host_data_out_io_address = request->request.cmd->nvme_cmd.dptr.prp.prp1;
	request->num_of_buffers = 1;
	request->host_buffer[0] = nvmf_doca_sq_get_host_buffer(request->doca_sq, host_data_out_io_address);
	request->dpu_buffer[0] = nvmf_doca_sq_get_dpu_buffer(request->doca_sq);
	doca_buf_get_head(request->dpu_buffer[0], &data_out_address);
	spdk_iov_one(request->request.iov, (int *)&request->request.iovcnt, data_out_address, request->request.length);
	request->request.data = data_out_address;
}

/*
 * Post CQE based on the NVMf response
 *
 * @request [in]: The NVMf request which holds the response
 * @arg [in]: Argument associated with the callback
 */
static void post_cqe_from_response(struct nvmf_doca_request *request, void *arg)
{
	(void)arg;

	union doca_data user_data;
	user_data.ptr = request;

	// Update SQ head
	request->request.rsp->nvme_cpl.sqhd = request->sqe_idx;

	nvmf_doca_io_post_cqe(request->doca_sq->io,
			      (const struct nvmf_doca_cqe *)&request->request.rsp->nvme_cpl,
			      user_data);
}

/*
 * Post CQE based on the NVMf response
 *
 * @request [in]: The NVMf request which holds the response
 */
static void post_error_cqe_from_response(struct nvmf_doca_request *request)
{
	request->request.rsp->nvme_cpl.cid = request->request.cmd->nvme_cmd.cid;
	request->request.rsp->nvme_cpl.status.sc = 1;

	post_cqe_from_response(request, request);
}

/*
 * Begin async operation of copying data from DPU to Host
 *
 * @request [in]: The NVMf request
 * @arg [in]: Argument associated with the callback
 */
static void copy_dpu_data_to_host(struct nvmf_doca_request *request, void *arg)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	(void)arg;
	union doca_data user_data;
	user_data.ptr = request;
	request->doca_cb = post_cqe_from_response;

	if (request->request.cmd->nvme_cmd.opc == SPDK_NVME_OPC_IDENTIFY) {
		struct spdk_nvme_ctrlr_data *cdata = (struct spdk_nvme_ctrlr_data *)request->request.data;

		/* Disable SGL */
		cdata->sgls.supported = SPDK_NVME_SGLS_NOT_SUPPORTED;
	}

	nvmf_doca_sq_copy_data(request->doca_sq,
			       request->host_buffer[0],
			       request->dpu_buffer[0],
			       request->request.length,
			       user_data);
}

/*
 * Begin async operation of copying data from DPU to Host for NVM commands
 *
 * @request [in]: The NVMf request
 * @arg [in]: Argument associated with the callback
 */
static void copy_nvme_dpu_data_to_host(struct nvmf_doca_request *request, void *arg)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	(void)arg;
	union doca_data user_data;
	user_data.ptr = request;
	request->doca_cb = post_cqe_from_response;

	uint32_t idx = request->num_of_buffers;

	for (idx = 0; idx < request->num_of_buffers; idx++) {
		nvmf_doca_sq_copy_data(request->doca_sq,
				       request->host_buffer[idx],
				       request->dpu_buffer[idx],
				       request->request.iov[idx].iov_len,
				       user_data);
	}
}

/*
 * Begin async operation of handling NVMe admin command that requires copying data back to Host
 *
 * @request [in]: The NVMf request
 */
static void begin_nvme_admin_cmd_data_dpu_to_host(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	// Prepare DPU data buffer
	init_dpu_host_buffers(request);

	request->doca_cb = copy_dpu_data_to_host;

	spdk_nvmf_request_exec(&request->request);
}

/*
 * Begin async operation of handling NVMe command that requires copying data back to Host for NVM commands
 *
 * @request [in]: The NVMf request
 */
static void begin_nvme_cmd_data_dpu_to_host(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	// Prepare DPU data buffer
	init_dpu_host_buffers(request);
}

/*
 * Copy data back to host onve the buffers have been inilialized and ready
 *
 * @request [in]: The request of NVM read
 */
static void buffers_ready_copy_data_dpu_to_host(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	request->doca_cb = copy_nvme_dpu_data_to_host;

	spdk_nvmf_request_exec(&request->request);
}

/*
 * Begin async NVMf request
 *
 * @request [in]: The NVMf request
 * @arg [in]: Argument associated with the callback
 */
static void execute_spdk_request(struct nvmf_doca_request *request, void *arg)
{
	(void)arg;

	request->doca_cb = post_cqe_from_response;
	spdk_nvmf_request_exec(&request->request);
}

/*
 * Begin async operation of handling NVMe admin command that requires copying data from Host
 *
 * @request [in]: The NVMf request
 */
static void begin_nvme_admin_cmd_data_host_to_dpu(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	union doca_data user_data;

	// Fetch data from Host to DPU
	init_dpu_host_buffers(request);
	user_data.ptr = request;
	request->doca_cb = execute_spdk_request;

	nvmf_doca_sq_copy_data(request->doca_sq,
			       request->dpu_buffer[0],
			       request->host_buffer[0],
			       request->request.length,
			       user_data);
}

/*
 * Begin async operation of handling NVMe command that requires copying data from Host
 *
 * @request [in]: The NVMf request
 */
static void begin_nvme_cmd_data_host_to_dpu(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	// Fetch data from Host to DPU
	init_dpu_host_buffers(request);
}

/*
 * Copy data to dpu once the buffers have been inilialized and ready
 *
 * @request [in]: The request of NVM write
 */
static void buffers_ready_copy_data_host_to_dpu(struct nvmf_doca_request *request)
{
	DOCA_LOG_TRC("Entering function %s", __func__);

	union doca_data user_data;

	user_data.ptr = request;
	request->doca_cb = execute_spdk_request;

	uint32_t idx = request->num_of_buffers;

	for (idx = 0; idx < request->num_of_buffers; idx++) {
		nvmf_doca_sq_copy_data(request->doca_sq,
				       request->dpu_buffer[idx],
				       request->host_buffer[idx],
				       request->request.iov[idx].iov_len,
				       user_data);
	}
}

/*
 * Begin async operation of handling NVMe command that does not require copy of data between Host and DPU
 *
 * @request [in]: The NVMf request
 */
static void begin_nvme_cmd_data_none(struct nvmf_doca_request *request)
{
	request->doca_cb = post_cqe_from_response;
	spdk_nvmf_request_exec(&request->request);
}

struct nvmf_doca_poll_group_create_io_cq_ctx {
	struct nvmf_doca_request *request;	       /**< The original create IO CQ admin command */
	struct nvmf_doca_pci_dev_admin *pci_dev_admin; /**< The PCI device admin context */
	struct nvmf_doca_poll_group *poll_group;       /**< Poll group where CQ should be created */
	struct nvmf_doca_io *io_cq;		       /**< The IO CQ to be initialized */
};

/*
 * Method to be called once async 'nvmf_doca_poll_group_create_io_cq()' completes
 *
 * @args [in]: The context of the async procedure
 */
static void nvmf_doca_poll_group_create_io_cq_done(void *args)
{
	struct nvmf_doca_poll_group_create_io_cq_ctx *ctx = args;
	struct nvmf_doca_request *request = ctx->request;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = ctx->pci_dev_admin;
	struct nvmf_doca_admin_qp *admin_qp = pci_dev_admin->admin_qp;
	struct nvmf_doca_io *io_cq = ctx->io_cq;

	/* If error then free io_cq */
	if (request->request.rsp->nvme_cpl.status.sc == 1) {
		free(io_cq);
	} else {
		TAILQ_INSERT_TAIL(&admin_qp->io_cqs, ctx->io_cq, pci_dev_admin_link);
	}

	free(ctx);

	request->request.rsp->nvme_cpl.cid = request->request.cmd->nvme_cmd.cid;
	post_cqe_from_response(request, request);
}

/*
 * Finds an IO SQ that matches a specific io_sq_id
 *
 * @admin_qp [in]: The PCI device admin QP context
 * @io_sq_id [in]: IO SQ ID
 * @return: A pointer to the maching SQ if found, or NULL if no match is found
 */
static struct nvmf_doca_sq *admin_qp_find_io_sq_by_id(struct nvmf_doca_admin_qp *admin_qp, uint32_t io_sq_id)
{
	struct nvmf_doca_sq *io_sq;
	TAILQ_FOREACH(io_sq, &admin_qp->io_sqs, pci_dev_admin_link)
	{
		if (io_sq->sq_id == io_sq_id) {
			return io_sq;
		}
	}
	return NULL;
}

/*
 * Callback invoked once IO SQ has been stopped
 *
 * @sq [in]: The NVMf DOCA SQ that was stopped
 */
static void nvmf_doca_on_io_sq_stop(struct nvmf_doca_sq *sq)
{
	struct spdk_thread *admin_qp_thread = sq->io->poll_group->pci_dev_admin->admin_qp_pg->pg.group->thread;

	nvmf_doca_io_rm_sq(sq);

	spdk_thread_exec_msg(admin_qp_thread, nvmf_doca_pci_dev_poll_group_stop_io_sq_done, sq);
}

/*
 * Finds a completion IO queue that matches a specific io_id
 *
 * @admin_qp [in]: The PCI device admin QP context
 * @io_cq_id [in]: IO CQ ID
 * @return: A pointer to the maching CQ if found, or NULL if no match is found
 */
static struct nvmf_doca_io *admin_qp_find_io_cq_by_id(struct nvmf_doca_admin_qp *admin_qp, uint32_t io_cq_id)
{
	struct nvmf_doca_io *io_cq;
	TAILQ_FOREACH(io_cq, &admin_qp->io_cqs, pci_dev_admin_link)
	{
		if (io_cq->cq.cq_id == io_cq_id) {
			return io_cq;
		}
	}
	return NULL;
}

/*
 * Callback invoked once IO CQ has been stopped
 *
 * @io [in]: The NVMf DOCA CQ that was stopped
 */
static void nvmf_doca_on_io_cq_stop(struct nvmf_doca_io *io)
{
	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg = io->poll_group;
	struct spdk_thread *admin_qp_thread = pci_dev_pg->pci_dev_admin->admin_qp_pg->pg.group->thread;

	TAILQ_REMOVE(&pci_dev_pg->io_cqs, io, pci_dev_pg_link);
	nvmf_doca_io_destroy(io);

	/**
	 * The PCI device poll group should be destroyed only after all CQs have been destroyed
	 * Admin QP thread is expected to poll admin CQs along with IO CQs
	 * In that case need to destroy the poll group only after admin CQ is destroyed
	 */
	if (pci_dev_pg->admin_qp == NULL && TAILQ_EMPTY(&pci_dev_pg->io_cqs)) {
		DOCA_LOG_INFO("Destroying PCI dev poll group %p", pci_dev_pg);
		TAILQ_REMOVE(&pci_dev_pg->poll_group->pci_dev_pg_list, pci_dev_pg, link);
		nvmf_doca_destroy_pci_dev_poll_group(pci_dev_pg);
		io->poll_group = NULL;
	}

	spdk_thread_exec_msg(admin_qp_thread, nvmf_doca_pci_dev_poll_group_stop_io_cq_done, io);
}

/*
 * Async Method to create an IO CQ, once complete 'nvmf_doca_poll_group_create_io_cq_done()' will be called
 *
 * @args [in]: The context of the async procedure
 */
static void nvmf_doca_poll_group_create_io_cq(void *args)
{
	doca_error_t ret;
	struct nvmf_doca_poll_group_create_io_cq_ctx *ctx = args;
	struct nvmf_doca_request *request = ctx->request;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = ctx->pci_dev_admin;
	struct nvmf_doca_poll_group *poll_group = ctx->poll_group;
	struct spdk_nvme_cmd *cmd = &request->request.cmd->nvme_cmd;
	uint16_t qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	/* If first CQ to be created on this poll group then create a PCI device poll group */
	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg = get_pci_dev_poll_group(poll_group, pci_dev_admin->pci_dev);
	if (pci_dev_pg == NULL) {
		ret = nvmf_doca_create_pci_dev_poll_group(pci_dev_admin, NULL, poll_group, &pci_dev_pg);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create PCI device poll group: %s", doca_error_get_name(ret));
			request->request.rsp->nvme_cpl.status.sc = 1;
			goto respond_to_admin;
		}
		TAILQ_INSERT_TAIL(&poll_group->pci_dev_pg_list, pci_dev_pg, link);
	}

	struct nvmf_doca_io_create_attr io_attr = {
		.pe = pci_dev_pg->poll_group->pe,
		.dev = pci_dev_admin->emulation_manager->emulation_manager,
		.nvme_dev = pci_dev_admin->pci_dev,
		.dpa = pci_dev_admin->emulation_manager->dpa,
		.cq_id = cmd->cdw10_bits.create_io_q.qid,
		.cq_depth = qsize,
		.host_cq_mmap = pci_dev_pg->host_mmap,
		.host_cq_address = cmd->dptr.prp.prp1,
		.msix_idx = cmd->cdw11_bits.create_io_cq.iv,
		.enable_msix = cmd->cdw11_bits.create_io_cq.ien,
		.max_num_sq = 64,
		.post_cqe_cb = nvmf_doca_on_post_nvm_cqe_complete,
		.fetch_sqe_cb = nvmf_doca_on_fetch_nvm_sqe_complete,
		.copy_data_cb = nvmf_doca_on_copy_nvm_data_complete,
		.stop_sq_cb = nvmf_doca_on_io_sq_stop,
		.stop_io_cb = nvmf_doca_on_io_cq_stop,
	};

	struct nvmf_doca_io *io_cq = ctx->io_cq;
	ret = nvmf_doca_io_create(&io_attr, io_cq);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create io: %s", doca_error_get_name(ret));
		request->request.rsp->nvme_cpl.status.sc = 1;
		goto respond_to_admin;
	}
	io_cq->poll_group = pci_dev_pg;
	io_cq->pci_dev_admin = pci_dev_admin;

	TAILQ_INSERT_TAIL(&pci_dev_pg->io_cqs, io_cq, pci_dev_pg_link);

respond_to_admin:;
	struct spdk_thread *admin_qp_thread = pci_dev_admin->admin_qp_pg->pg.group->thread;
	spdk_thread_exec_msg(admin_qp_thread, nvmf_doca_poll_group_create_io_cq_done, args);
}

/*
 * Creates a completion I/O queue
 *
 * @sq [in]: The SQ that holds the command
 * @request [in]: the NVME command
 */
static void handle_create_io_cq(struct nvmf_doca_sq *sq, struct nvmf_doca_request *request)
{
	struct nvmf_doca_io *io_cq = calloc(1, sizeof(*io_cq));
	if (io_cq == NULL) {
		DOCA_LOG_ERR("Failed to create IO CQ: Out of memory");
		post_error_cqe_from_response(request);
		return;
	}

	struct nvmf_doca_pci_dev_admin *pci_dev_admin = sq->io->poll_group->pci_dev_admin;
	struct nvmf_doca_poll_group *poll_group = choose_poll_group(pci_dev_admin->doca_transport);
	struct nvmf_doca_poll_group_create_io_cq_ctx *create_io_cq_ctx = calloc(1, sizeof(*create_io_cq_ctx));
	if (create_io_cq_ctx == NULL) {
		DOCA_LOG_ERR("Failed to create IO CQ: Out of memory");
		free(io_cq);
		post_error_cqe_from_response(request);
		return;
	}
	*create_io_cq_ctx = (struct nvmf_doca_poll_group_create_io_cq_ctx){
		.request = request,
		.pci_dev_admin = pci_dev_admin,
		.poll_group = poll_group,
		.io_cq = io_cq,
	};

	struct spdk_thread *thread = poll_group->pg.group->thread;
	spdk_thread_exec_msg(thread, nvmf_doca_poll_group_create_io_cq, create_io_cq_ctx);
}

/*
 * Handle delete IO CQ admin command
 *
 * @sq [in]: The SQ that holds the command
 * @request [in]: The NVMe command
 */
static void handle_delete_io_cq(struct nvmf_doca_sq *sq, struct nvmf_doca_request *request)
{
	struct nvmf_doca_admin_qp *admin_qp = sq->io->poll_group->admin_qp;

	uint32_t io_cq_id = request->request.cmd->nvme_cmd.cdw10_bits.delete_io_q.qid;
	struct nvmf_doca_io *io_cq = admin_qp_find_io_cq_by_id(admin_qp, io_cq_id);
	if (io_cq == NULL) {
		DOCA_LOG_ERR("Failed to delete IO CQ: IO CQ with ID %u does not exist", io_cq_id);
		post_error_cqe_from_response(request);
		return;
	}

	struct nvmf_doca_poll_group_delete_io_cq_ctx *delete_io_cq_ctx = calloc(1, sizeof(*delete_io_cq_ctx));
	if (delete_io_cq_ctx == NULL) {
		DOCA_LOG_ERR("Failed to delete IO CQ: Out of memory");
		post_error_cqe_from_response(request);
		return;
	}
	*delete_io_cq_ctx = (struct nvmf_doca_poll_group_delete_io_cq_ctx){
		.request = request,
	};
	io_cq->ctx = delete_io_cq_ctx;

	struct spdk_thread *thread = io_cq->poll_group->poll_group->pg.group->thread;
	spdk_thread_exec_msg(thread, nvmf_doca_pci_dev_poll_group_stop_io_cq, io_cq);
}

/*
 * Method to be called once async 'nvmf_doca_poll_group_create_io_sq()' completes
 *
 * @args [in]: The context of the async procedure
 */
static void nvmf_doca_poll_group_create_io_sq_done(void *args)
{
	struct nvmf_doca_poll_group_create_io_sq_ctx *ctx = args;
	struct nvmf_doca_request *request = ctx->request;
	struct nvmf_doca_sq *io_sq = ctx->io_sq;
	struct nvmf_doca_admin_qp *admin_qp = io_sq->io->poll_group->pci_dev_admin->admin_qp;

	/* If error then free io_cq */
	if (request->request.rsp->nvme_cpl.status.sc == 1) {
		free(io_sq);
	} else {
		TAILQ_INSERT_TAIL(&admin_qp->io_sqs, io_sq, pci_dev_admin_link);
	}

	free(ctx);

	request->request.rsp->nvme_cpl.cid = request->request.cmd->nvme_cmd.cid;
	post_cqe_from_response(request, request);
}

/*
 * Async Method to create an IO SQ, once complete 'nvmf_doca_poll_group_create_io_sq_done()' will be called
 *
 * @args [in]: The context of the async procedure
 */
static void nvmf_doca_poll_group_create_io_sq(void *args)
{
	struct nvmf_doca_poll_group_create_io_sq_ctx *ctx = args;
	struct nvmf_doca_request *request = ctx->request;
	struct spdk_nvme_cmd *cmd = &request->request.cmd->nvme_cmd;
	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg = ctx->pci_dev_pg;
	struct nvmf_doca_pci_dev_admin *pci_dev_admin = pci_dev_pg->pci_dev_admin;
	uint32_t qsize = cmd->cdw10_bits.create_io_q.qsize + 1;

	struct nvmf_doca_io_add_sq_attr sq_attr = {
		.pe = pci_dev_pg->poll_group->pe,
		.dev = pci_dev_admin->emulation_manager->emulation_manager,
		.nvme_dev = pci_dev_admin->pci_dev,
		.sq_depth = qsize,
		.host_sq_mmap = pci_dev_pg->host_mmap,
		.host_sq_address = cmd->dptr.prp.prp1,
		.sq_id = cmd->cdw10_bits.create_io_q.qid,
		.transport = pci_dev_pg->poll_group->pg.transport,
		.ctx = args,
	};
	nvmf_doca_io_add_sq(ctx->io_cq, &sq_attr, ctx->io_sq);
}

/*
 * Creates a submision I/O queue
 *
 * @sq [in]: The SQ that holds the command
 * @request [in]: the NVME command
 */
static void handle_create_io_sq(struct nvmf_doca_sq *sq, struct nvmf_doca_request *request)
{
	struct nvmf_doca_admin_qp *admin_qp = sq->io->poll_group->admin_qp;
	uint32_t io_cq_id = request->request.cmd->nvme_cmd.cdw10_bits.create_io_q.qid;
	struct nvmf_doca_io *io_cq = admin_qp_find_io_cq_by_id(admin_qp, io_cq_id);
	if (io_cq == NULL) {
		DOCA_LOG_ERR("Failed to create IO SQ: IO CQ with ID %u not found", io_cq_id);
		post_error_cqe_from_response(request);
		return;
	}

	struct nvmf_doca_pci_dev_poll_group *pci_dev_pg = io_cq->poll_group;
	struct spdk_thread *thread = pci_dev_pg->poll_group->pg.group->thread;
	struct nvmf_doca_sq *io_sq = calloc(1, sizeof(*io_sq));
	if (io_sq == NULL) {
		DOCA_LOG_ERR("Failed to create IO SQ: Out of memory");
		post_error_cqe_from_response(request);
		return;
	}

	struct nvmf_doca_poll_group_create_io_sq_ctx *create_io_sq_ctx = calloc(1, sizeof(*create_io_sq_ctx));
	if (create_io_sq_ctx == NULL) {
		DOCA_LOG_ERR("Failed to create IO SQ: Out of memory");
		free(io_sq);
		post_error_cqe_from_response(request);
		return;
	}
	*create_io_sq_ctx = (struct nvmf_doca_poll_group_create_io_sq_ctx){
		.request = request,
		.pci_dev_pg = pci_dev_pg,
		.io_cq = io_cq,
		.io_sq = io_sq,
	};

	spdk_thread_exec_msg(thread, nvmf_doca_poll_group_create_io_sq, create_io_sq_ctx);
}

/*
 * Handle delete IO SQ admin command
 *
 * @sq [in]: The SQ that holds the command
 * @request [in]: The NVMe command
 */
static void handle_delete_io_sq(struct nvmf_doca_sq *sq, struct nvmf_doca_request *request)
{
	struct nvmf_doca_admin_qp *admin_qp = sq->io->poll_group->admin_qp;
	uint32_t io_sq_id = request->request.cmd->nvme_cmd.cdw10_bits.delete_io_q.qid;

	struct nvmf_doca_sq *io_sq = admin_qp_find_io_sq_by_id(admin_qp, io_sq_id);
	if (io_sq == NULL) {
		DOCA_LOG_ERR("Failed to delete IO SQ: IO SQ with ID %u does not exist", io_sq_id);
		post_error_cqe_from_response(request);
		return;
	}

	struct nvmf_doca_poll_group_delete_io_sq_ctx *delete_io_sq_ctx = calloc(1, sizeof(*delete_io_sq_ctx));
	if (delete_io_sq_ctx == NULL) {
		DOCA_LOG_ERR("Failed to delete IO SQ: Out of memory");
		post_error_cqe_from_response(request);
		return;
	}
	*delete_io_sq_ctx = (struct nvmf_doca_poll_group_delete_io_sq_ctx){
		.request = request,
	};
	io_sq->ctx = delete_io_sq_ctx;

	struct spdk_thread *thread = io_sq->io->poll_group->poll_group->pg.group->thread;
	spdk_thread_exec_msg(thread, nvmf_doca_pci_dev_poll_group_stop_io_sq, io_sq);
}

/*
 * Callback invoked once SQE has been fetched from Host SQ
 *
 * @sq [in]: The SQ used for the fetch operation
 * @sqe [in]: The SQE that was fetched from Host
 * @sqe_idx [in]: The SQE index
 */
static void nvmf_doca_on_fetch_sqe_complete(struct nvmf_doca_sq *sq, struct nvmf_doca_sqe *sqe, uint16_t sqe_idx)
{
	// Prepare request
	struct spdk_nvme_cmd *cmd = (struct spdk_nvme_cmd *)&sqe->data[0];
	struct nvmf_doca_request *request = nvmf_doca_request_get(sq);
	request->request.cmd->nvme_cmd = *cmd;
	request->sqe_idx = sqe_idx;
	request->doca_sq = sq;

	request->request.xfer = spdk_nvme_opc_get_data_transfer(request->request.cmd->nvme_cmd.opc);

	DOCA_LOG_DBG("Received admin command: opcode %u", request->request.cmd->nvme_cmd.opc);
	switch (request->request.cmd->nvme_cmd.opc) {
	case SPDK_NVME_OPC_CREATE_IO_CQ:
		handle_create_io_cq(sq, request);
		return;
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		handle_delete_io_cq(sq, request);
		return;
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		handle_create_io_sq(sq, request);
		return;
	case SPDK_NVME_OPC_DELETE_IO_SQ:
		handle_delete_io_sq(sq, request);
		return;
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		request->request.length = 0;
		request->request.xfer = SPDK_NVME_DATA_NONE;
		break;
	case SPDK_NVME_OPC_IDENTIFY:
		request->request.length = IDENTIFY_CMD_DATA_BUFFER_SIZE;
		break;
	case SPDK_NVME_OPC_GET_LOG_PAGE:;
		uint32_t num_dword =
			((((uint32_t)cmd->cdw11_bits.get_log_page.numdu << 16) | cmd->cdw10_bits.get_log_page.numdl) +
			 1);
		if (num_dword > UINT32_MAX / 4) {
			DOCA_LOG_ERR("NUMD exceeds maximum size: num of DW %u", num_dword);
			break;
		}
		request->request.length = num_dword * 4;
		break;
	case SPDK_NVME_OPC_GET_FEATURES:
	case SPDK_NVME_OPC_SET_FEATURES:;
		uint8_t fid = cmd->cdw10_bits.set_features.fid;
		DOCA_LOG_DBG("Received feature: opcode %u", fid);
		switch (fid) {
		case SPDK_NVME_FEAT_LBA_RANGE_TYPE:
			request->request.length = FEAT_CMD_LBA_RANGE_SIZE;
			break;
		case SPDK_NVME_FEAT_AUTONOMOUS_POWER_STATE_TRANSITION:
			request->request.length = FEAT_CMD_AUTONOMOUS_POWER_STATE_TRANSITION_SIZE;
			break;
		case SPDK_NVME_FEAT_TIMESTAMP:
			request->request.length = FEAT_CMD_TIMESTAMP_SIZE;
			break;
		case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
			request->request.length = FEAT_CMD_HOST_BEHAVIOR_SUPPORT_SIZE;
			break;
		case SPDK_NVME_FEAT_HOST_IDENTIFIER:
			if (cmd->cdw11_bits.feat_host_identifier.bits.exhid) {
				request->request.length = FEAT_CMD_HOST_IDENTIFIER_EXT_SIZE;
			} else {
				request->request.length = FEAT_CMD_HOST_IDENTIFIER_SIZE;
			}
			break;
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		case SPDK_NVME_FEAT_ARBITRATION:
		case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		case SPDK_NVME_FEAT_ERROR_RECOVERY:
		case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
		case SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION:
		case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		case SPDK_NVME_FEAT_HOST_MEM_BUFFER:
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		case SPDK_NVME_FEAT_HOST_CONTROLLED_THERMAL_MANAGEMENT:
		case SPDK_NVME_FEAT_NON_OPERATIONAL_POWER_STATE_CONFIG:
		case SPDK_NVME_FEAT_READ_RECOVERY_LEVEL_CONFIG:
		case SPDK_NVME_FEAT_PREDICTABLE_LATENCY_MODE_CONFIG: /* this feature is supposed to have a data buffer*/
		case SPDK_NVME_FEAT_PREDICTABLE_LATENCY_MODE_WINDOW:
		case SPDK_NVME_FEAT_LBA_STATUS_INFORMATION_ATTRIBUTES:
		case SPDK_NVME_FEAT_SANITIZE_CONFIG:
		case SPDK_NVME_FEAT_ENDURANCE_GROUP_EVENT:
		case SPDK_NVME_FEAT_SOFTWARE_PROGRESS_MARKER:
		case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
			request->request.length = 0;
			request->request.xfer = SPDK_NVME_DATA_NONE;
			break;
		default:
			DOCA_LOG_ERR("Received unsupported feautre: opcode %u", fid);
			return;
		}
		break;
	default:
		DOCA_LOG_ERR("Received unsupported command: opcode %u", cmd->opc);
		return;
	}

	// Determine data direction
	switch (request->request.xfer) {
	case SPDK_NVME_DATA_NONE:
		/**
		 * This will begin an async flow passing through the following methods
		 * begin_nvme_cmd_data_none ---> post_cqe_from_response ---> nvmf_doca_on_post_cqe_complete
		 */
		begin_nvme_cmd_data_none(request);
		break;
	case SPDK_NVME_DATA_HOST_TO_CONTROLLER:
		/**
		 * This will begin an async flow passing through the following methods
		 * begin_nvme_admin_cmd_data_host_to_dpu ---> execute_spdk_request ---> post_cqe_from_response --->
		 * nvmf_doca_on_post_cqe_complete
		 */
		begin_nvme_admin_cmd_data_host_to_dpu(request);
		break;
	case SPDK_NVME_DATA_CONTROLLER_TO_HOST:
		/**
		 * This will begin an async flow passing through the following methods
		 * begin_nvme_admin_cmd_data_dpu_to_host ---> copy_dpu_data_to_host ---> post_cqe_from_response --->
		 * nvmf_doca_on_post_cqe_complete
		 */
		begin_nvme_admin_cmd_data_dpu_to_host(request);
		break;
	case SPDK_NVME_DATA_BIDIRECTIONAL:
		DOCA_LOG_ERR("Command with bidirectional data not support");
		return;
	default:
		DOCA_LOG_ERR("Received unidentified data direction");
		return;
	}
}

#define LBA_SIZE 512

/*
 * Callback invoked once NVM IO SQE has been fetched from Host SQ
 *
 * @sq [in]: The SQ used for the fetch operation
 * @sqe [in]: The SQE that was fetched from Host
 * @sqe_idx [in]: The SQE index
 */
static void nvmf_doca_on_fetch_nvm_sqe_complete(struct nvmf_doca_sq *sq, struct nvmf_doca_sqe *sqe, uint16_t sqe_idx)
{
	// Prepare request
	struct spdk_nvme_cmd *cmd = (struct spdk_nvme_cmd *)&sqe->data[0];
	struct nvmf_doca_request *request = nvmf_doca_request_get(sq);

	request->request.cmd = (union nvmf_h2c_msg *)cmd;
	request->doca_sq = sq;
	request->sqe_idx = sqe_idx;
	request->request.xfer = spdk_nvme_opc_get_data_transfer(request->request.cmd->nvme_cmd.opc);

	DOCA_LOG_DBG("Received NVMe command: opcode %u", request->request.cmd->nvme_cmd.opc);
	switch (request->request.cmd->nvme_cmd.opc) {
	case SPDK_NVME_OPC_FLUSH:
		break;
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_READ:
		request->request.length = (request->request.cmd->nvme_cmd.cdw12_bits.copy.nr + 1) * LBA_SIZE;
		break;
	default:
		DOCA_LOG_ERR("Received unsupported NVM command: opcode %u", cmd->opc);
		post_error_cqe_from_response(request);
		return;
	}

	// Determine data direction
	switch (request->request.xfer) {
	case SPDK_NVME_DATA_NONE:
		/**
		 * This will begin an async flow passing through the following methods
		 * begin_nvme_cmd_data_none ---> post_cqe_from_response ---> nvmf_doca_on_post_cqe_complete
		 */
		begin_nvme_cmd_data_none(request);
		break;
	case SPDK_NVME_DATA_HOST_TO_CONTROLLER:
		/**
		 * This will begin an async flow passing through the following methods
		 * begin_nvme_cmd_data_host_to_dpu ---> nvme_cmd_map_prps ---> buffers_ready_copy_data_host_to_dpu
		 * --> execute_spdk_request ---> post_cqe_from_response ---> nvmf_doca_on_post_cqe_complete
		 */
		begin_nvme_cmd_data_host_to_dpu(request);
		break;
	case SPDK_NVME_DATA_CONTROLLER_TO_HOST:
		/**
		 * This will begin an async flow passing through the following methods
		 * begin_nvme_cmd_data_dpu_to_host ---> nvme_cmd_map_prps ---> buffers_ready_copy_data_dpu_to_host
		 * ---> copy_dpu_data_to_host ---> post_cqe_from_response ---> nvmf_doca_on_post_cqe_complete
		 */
		begin_nvme_cmd_data_dpu_to_host(request);
		break;
	case SPDK_NVME_DATA_BIDIRECTIONAL:
		DOCA_LOG_ERR("Command with bidirectional data not support");
		return;
	default:
		DOCA_LOG_ERR("Received unidentified data direction");
		return;
	}
}

/*
 * Callback invoked once CQE has been posted to Host CQ
 *
 * @cq [in]: The CQ used for the post operation
 * @user_data [in]: Same user data previously provided in 'nvmf_doca_io_post_cqe()'
 */
static void nvmf_doca_on_post_cqe_complete(struct nvmf_doca_cq *cq, union doca_data user_data)
{
	(void)cq;

	struct nvmf_doca_request *request = user_data.ptr;

	nvmf_doca_req_free(&request->request);
}

/*
 * Callback invoked once CQE has been posted to Host IO CQ
 *
 * @cq [in]: The CQ used for the post operation
 * @user_data [in]: Same user data previously provided in 'nvmf_doca_io_post_cqe()'
 */
static void nvmf_doca_on_post_nvm_cqe_complete(struct nvmf_doca_cq *cq, union doca_data user_data)
{
	(void)cq;

	struct nvmf_doca_request *request = user_data.ptr;

	nvmf_doca_req_free(&request->request);
}

/*
 * Callback invoked once data copied to Host.
 *
 * The source and destination buffers must be freed at some point using doca_buf_dec_refcount()
 *
 * @sq [in]: The SQ used for the copy operation
 * @dst [in]: The buffer used as destination in the copy operation
 * @src [in]: The buffer used as source in the copy operation
 * @user_data [in]: Same user data previously provided in nvmf_doca_sq_copy_data()
 */
static void nvmf_doca_on_copy_data_complete(struct nvmf_doca_sq *sq,
					    struct doca_buf *dst,
					    struct doca_buf *src,
					    union doca_data user_data)
{
	(void)sq;
	(void)dst;
	(void)src;

	struct nvmf_doca_request *request = user_data.ptr;

	nvmf_doca_request_complete(request);
}

/*
 * Callback invoked once NVM command data copied to Host.
 *
 * The source and destination buffers must be freed at some point using doca_buf_dec_refcount()
 *
 * @sq [in]: The SQ used for the copy operation
 * @dst [in]: The buffer used as destination in the copy operation
 * @src [in]: The buffer used as source in the copy operation
 * @user_data [in]: Same user data previously provided in nvmf_doca_sq_copy_data()
 */
static void nvmf_doca_on_copy_nvm_data_complete(struct nvmf_doca_sq *sq,
						struct doca_buf *dst,
						struct doca_buf *src,
						union doca_data user_data)
{
	(void)sq;
	(void)dst;
	(void)src;

	struct nvmf_doca_request *request = user_data.ptr;
	request->num_of_buffers--;
	if (request->num_of_buffers == 0)
		nvmf_doca_request_complete(request);
}

/**
 * Implementation of the DOCA transport
 */
const struct spdk_nvmf_transport_ops spdk_nvmf_transport_doca = {
	.name = "DOCA",
	.type = SPDK_NVME_TRANSPORT_CUSTOM,
	.opts_init = nvmf_doca_opts_init,
	.create = nvmf_doca_create,
	.dump_opts = nvmf_doca_dump_opts,
	.destroy = nvmf_doca_destroy,

	.listen = nvmf_doca_listen,
	.stop_listen = nvmf_doca_stop_listen,
	.listen_associate = nvmf_doca_listen_associate,

	.poll_group_create = nvmf_doca_poll_group_create,
	.get_optimal_poll_group = nvmf_doca_get_optimal_poll_group,
	.poll_group_destroy = nvmf_doca_poll_group_destroy,
	.poll_group_add = nvmf_doca_poll_group_add,
	.poll_group_remove = nvmf_doca_poll_group_remove,
	.poll_group_poll = nvmf_doca_poll_group_poll,

	.req_free = nvmf_doca_req_free,
	.req_complete = nvmf_doca_req_complete,

	.qpair_fini = nvmf_doca_close_qpair,
	.qpair_get_listen_trid = nvmf_doca_qpair_get_listen_trid,
};

SPDK_NVMF_TRANSPORT_REGISTER(doca, &spdk_nvmf_transport_doca);
