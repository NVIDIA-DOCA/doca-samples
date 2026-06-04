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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <linux/if_ether.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_dev.h>
#include <doca_ctx.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_tlp.h>
#include <doca_devemu_pci_info.h>
#include <doca_devemu_vnet_type.h>
#include <doca_devemu_vnet.h>
#include <doca_devemu_vnet_offload_engine.h>
#include <doca_devemu_virtio.h>
#include <doca_devemu_virtio_tlp.h>
#include <doca_bitfield.h>

#include "vnet_pci_device.h"
#include "vnet_pci_dev_core.h"
#include "pci_spec_tlp.h"
#include <doca_devemu_virtio.h>

DOCA_LOG_REGISTER(VNET_PCI_DEVICE);

/************************************************************************
 ******                  Workqueue Infrastructure                  ******
 ************************************************************************/

#define VNET_TLP_REQ_USER_DATA_SIZE 128 /* Per-request opaque storage for TLP handler callbacks */
#define PCI_CFG_WORK_POOL_SIZE 1024	/* Pre-allocated work items - avoids malloc in hot path */
#define MQ_START_MAX_CONCURRENT \
	2 /* Max concurrent start_additional_queue_pairs threads. \
	   * Each MQ start for 127 QPs is very memory-intensive \
	   * (OOM risk) and saturates firmware command channel \
	   * (PE1 starvation risk -> host PCIe timeout). \
	   * Keep low to balance throughput vs. stability. */

/* Work item types */
enum pci_cfg_work_type {
	PCI_CFG_WORK_VQ_CONFIG,
	PCI_CFG_WORK_CONTROLLER_CLEANUP,
	PCI_CFG_WORK_ENGINE_START,
	PCI_CFG_WORK_INITIALIZE_VQS,
	PCI_CFG_WORK_INITIALIZE_IO_CTX,
	PCI_CFG_WORK_START_AND_ENABLE,
	PCI_CFG_WORK_CREATE_STATS_LIST,
	PCI_CFG_WORK_DEVICE_RESET,
	PCI_CFG_WORK_DELAYED_DESTROY,
	PCI_CFG_WORK_HOTPLUG_EVENT,
	PCI_CFG_WORK_SPEED_CHANGE,
};

/* Work item for PCI config operations */
struct pci_cfg_work_item {
	enum pci_cfg_work_type type;
	struct vnet_pci_device *dev;
	union {
		struct {
			uint16_t vq_index;
		} vq_config;
		struct {
			struct vnet_pci_dev_controller *controller;
			bool destroy_engine;
		} controller_cleanup;
		struct {
			struct vnet_pci_dev_controller *controller;
		} engine_start;
		struct {
			struct vnet_pci_dev_controller *controller;
		} initialize_vqs;
		struct {
			struct vnet_pci_dev_controller *controller;
		} initialize_io_ctx;
		struct {
			struct vnet_pci_dev_controller *controller;
		} start_and_enable;
		struct {
			struct vnet_pci_dev_controller *controller;
		} create_stats_list;
		struct {
			struct vnet_pci_dev_controller *controller;
			uint64_t driver_features; /* Negotiated features */
		} features_ok;
		struct {
			struct vnet_pci_dev_controller *controller;
		} driver_ok;
		struct {
			struct tlp_context *tlp_ctx;
			struct pci_device_config *endpoint;
		} delayed_destroy;
		struct {
			struct tlp_context *tlp_ctx;
			uint32_t dsp_index;
			bool plug;
		} hotplug_event;
		struct {
			struct vnet_pci_dev_controller *controller;
			uint32_t new_speed;
		} speed_change;
	} data;
	struct pci_cfg_work_item *next;
	uint32_t reset_generation;
	bool in_use; /* Pool allocation flag */
};

/* Pre-allocated work item pool */
struct pci_cfg_work_pool {
	struct pci_cfg_work_item items[PCI_CFG_WORK_POOL_SIZE];
	pthread_spinlock_t lock;
	uint32_t free_count;
};

struct mq_start_thread_node {
	pthread_t thread;
	bool completed;
	struct mq_start_thread_node *next;
};

/* Workqueue structure */
struct pci_cfg_workqueue {
	pthread_t worker_thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_cond_t reset_cond; /* Signaled when worker idle or reset completes */
	struct pci_cfg_work_item *head;
	struct pci_cfg_work_item *tail;
	volatile bool running;
	volatile bool shutdown;
	volatile bool reset_in_progress;	    /* Blocks VQ config submissions during device reset */
	volatile bool worker_busy;		    /* Worker is processing a work item */
	struct vnet_pci_device *worker_current_dev; /* Device the worker is currently operating on
						     * (NULL for device-independent work like hotplug) */
	struct pci_cfg_work_pool pool;		    /* Pre-allocated work item pool */
	sem_t mq_start_sem;			    /* Limits concurrent MQ start threads to avoid OOM */
	bool mq_start_teardown;			    /* Prevents MQ threads from using destroyed state */
	struct mq_start_thread_node *mq_threads;    /* Joinable MQ start threads owned by this workqueue */
	uint32_t mq_thread_count;
	/* PE2: Worker PEs for async ops - one per controller.
	 * Multi-endpoint mode creates per-controller worker PEs and IO contexts
	 * are attached to their respective PEs. We must progress ALL worker PEs
	 * to avoid stalling controllers 1..N. */
	struct doca_pe *worker_pes[MAX_NUM_EP];
	uint32_t worker_pe_count;
	struct tlp_context *diag_tlp_ctx; /* Controllers used for worker-side diagnostics */
	bool diag_collection_enabled;
	uint32_t diag_next_controller;
	/* Statistics */
	uint32_t pending_work_count;
	uint64_t total_processed;
	uint64_t dropped_count; /* Items dropped due to pool exhaustion or shutdown */
	uint64_t vq_config_count;
	uint64_t controller_cleanup_count;
	uint64_t engine_start_count;
	uint64_t initialize_vqs_count;
	uint64_t initialize_io_ctx_count;
	uint64_t start_and_enable_count;
	uint64_t create_stats_list_count;
	uint64_t device_reset_count;
	uint64_t delayed_destroy_count;
};

/* Global workqueue instance */
static struct pci_cfg_workqueue *pci_cfg_wq = NULL;
static atomic_uint mq_start_threads_active;
static pthread_mutex_t vnet_affinity_mutex = PTHREAD_MUTEX_INITIALIZER;
static cpu_set_t vnet_affinity_base_set;
static bool vnet_affinity_base_valid;
static int vnet_affinity_tlp_core_idx = -1;
static int vnet_affinity_worker_core_idx = -1;
static int vnet_affinity_mq_core_idx = -1;

enum vnet_thread_affinity_role {
	VNET_THREAD_AFFINITY_MAIN,
	VNET_THREAD_AFFINITY_WORKER,
	VNET_THREAD_AFFINITY_MQ,
};

static void vnet_affinity_core_to_string(int core_idx, char *buffer, size_t buffer_len)
{
	if (core_idx < 0)
		snprintf(buffer, buffer_len, "auto");
	else
		snprintf(buffer, buffer_len, "%d", core_idx);
}

void vnet_pci_device_configure_affinity(int tlp_core_idx, int worker_core_idx, int mq_core_idx)
{
	char tlp_core[16];
	char worker_core[16];
	char mq_core[16];

	vnet_affinity_tlp_core_idx = tlp_core_idx;
	vnet_affinity_worker_core_idx = worker_core_idx;
	vnet_affinity_mq_core_idx = mq_core_idx;
	vnet_affinity_core_to_string(tlp_core_idx, tlp_core, sizeof(tlp_core));
	vnet_affinity_core_to_string(worker_core_idx, worker_core, sizeof(worker_core));
	vnet_affinity_core_to_string(mq_core_idx, mq_core, sizeof(mq_core));
	DOCA_LOG_INFO("Thread affinity configured: tlp_core=%s worker_core=%s mq_core=%s",
		      tlp_core,
		      worker_core,
		      mq_core);
}

static bool vnet_affinity_init_base_set(void)
{
	int ret;

	pthread_mutex_lock(&vnet_affinity_mutex);
	if (vnet_affinity_base_valid) {
		pthread_mutex_unlock(&vnet_affinity_mutex);
		return true;
	}

	ret = sched_getaffinity(0, sizeof(vnet_affinity_base_set), &vnet_affinity_base_set);
	if (ret != 0) {
		pthread_mutex_unlock(&vnet_affinity_mutex);
		DOCA_LOG_WARN("Failed to query base CPU affinity: %d", errno);
		return false;
	}

	vnet_affinity_base_valid = true;
	pthread_mutex_unlock(&vnet_affinity_mutex);
	return true;
}

static uint32_t vnet_affinity_get_base_cpus(int *cpus, uint32_t cpus_len)
{
	uint32_t count = 0;
	bool cpu0_allowed = false;
	int cpu;

	if (!vnet_affinity_init_base_set())
		return 0;

	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &vnet_affinity_base_set)) {
			if (cpu == 0)
				cpu0_allowed = true;
		}
	}

	for (cpu = 0; cpu < CPU_SETSIZE && count < cpus_len; cpu++) {
		if (cpu == 0)
			continue;
		if (CPU_ISSET(cpu, &vnet_affinity_base_set)) {
			cpus[count] = cpu;
			count++;
		}
	}
	if (cpu0_allowed && count < cpus_len) {
		cpus[count] = 0;
		count++;
	}

	return count;
}

static void vnet_affinity_format_cpu_set(const cpu_set_t *cpu_set, char *buffer, size_t buffer_len)
{
	size_t used = 0;
	bool first = true;
	int cpu;

	if (buffer_len == 0)
		return;

	buffer[0] = '\0';
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		int written;

		if (!CPU_ISSET(cpu, cpu_set))
			continue;

		written = snprintf(buffer + used, buffer_len - used, "%s%d", first ? "" : ",", cpu);
		if (written < 0 || (size_t)written >= buffer_len - used)
			break;

		used += (size_t)written;
		first = false;
	}

	if (first)
		snprintf(buffer, buffer_len, "none");
}

static bool vnet_affinity_cpu_is_excluded(int cpu,
					  int excluded_core,
					  int secondary_excluded_core,
					  int tertiary_excluded_core)
{
	return cpu == excluded_core || cpu == secondary_excluded_core || cpu == tertiary_excluded_core;
}

static bool vnet_affinity_select_cpu(const int *cpus,
				     uint32_t cpu_count,
				     int excluded_core,
				     int secondary_excluded_core,
				     int tertiary_excluded_core,
				     uint32_t start_index,
				     int *selected_cpu)
{
	uint32_t i;

	for (i = start_index; i < cpu_count; i++) {
		if (vnet_affinity_cpu_is_excluded(cpus[i],
						  excluded_core,
						  secondary_excluded_core,
						  tertiary_excluded_core))
			continue;
		*selected_cpu = cpus[i];
		return true;
	}

	for (i = 0; i < start_index && i < cpu_count; i++) {
		if (vnet_affinity_cpu_is_excluded(cpus[i],
						  excluded_core,
						  secondary_excluded_core,
						  tertiary_excluded_core))
			continue;
		*selected_cpu = cpus[i];
		return true;
	}

	return false;
}

static void vnet_pci_device_pin_current_thread(enum vnet_thread_affinity_role role, const char *thread_name)
{
	int cpus[CPU_SETSIZE];
	cpu_set_t target_set;
	char cpu_list[128];
	uint32_t cpu_count;
	bool explicit_affinity = false;
	int peer_explicit_core = -1;
	int selected_cpu;
	int ret;

	cpu_count = vnet_affinity_get_base_cpus(cpus, CPU_SETSIZE);
	if (cpu_count == 0)
		return;

	CPU_ZERO(&target_set);
	switch (role) {
	case VNET_THREAD_AFFINITY_MAIN:
		if (vnet_affinity_tlp_core_idx >= 0) {
			CPU_SET(vnet_affinity_tlp_core_idx, &target_set);
			explicit_affinity = true;
		} else {
			peer_explicit_core = vnet_affinity_worker_core_idx;
			if (!vnet_affinity_select_cpu(cpus,
						      cpu_count,
						      peer_explicit_core,
						      vnet_affinity_mq_core_idx,
						      -1,
						      (cpu_count > 1) ? 1 : 0,
						      &selected_cpu))
				selected_cpu = cpus[0];
			CPU_SET(selected_cpu, &target_set);
		}
		break;
	case VNET_THREAD_AFFINITY_WORKER:
		if (vnet_affinity_worker_core_idx >= 0) {
			CPU_SET(vnet_affinity_worker_core_idx, &target_set);
			explicit_affinity = true;
		} else {
			peer_explicit_core = vnet_affinity_tlp_core_idx;
			if (!vnet_affinity_select_cpu(cpus,
						      cpu_count,
						      peer_explicit_core,
						      vnet_affinity_mq_core_idx,
						      -1,
						      (cpu_count > 2) ? 2 : 0,
						      &selected_cpu)) {
				selected_cpu = cpus[(cpu_count > 1) ? 1 : 0];
			}
			CPU_SET(selected_cpu, &target_set);
		}
		break;
	case VNET_THREAD_AFFINITY_MQ:
		if (vnet_affinity_mq_core_idx >= 0) {
			CPU_SET(vnet_affinity_mq_core_idx, &target_set);
			explicit_affinity = true;
		} else {
			if (!vnet_affinity_select_cpu(cpus,
						      cpu_count,
						      vnet_affinity_tlp_core_idx,
						      vnet_affinity_worker_core_idx,
						      -1,
						      (cpu_count > 3) ? 3 : 0,
						      &selected_cpu)) {
				selected_cpu = cpus[(cpu_count > 1) ? 1 : 0];
			}
			CPU_SET(selected_cpu, &target_set);
		}
		break;
	}

	ret = pthread_setaffinity_np(pthread_self(), sizeof(target_set), &target_set);
	if (ret != 0) {
		DOCA_LOG_WARN("Failed to pin %s thread affinity: %d", thread_name, ret);
		return;
	}

	vnet_affinity_format_cpu_set(&target_set, cpu_list, sizeof(cpu_list));
	if (!explicit_affinity && cpu_count < 4) {
		DOCA_LOG_WARN("%s thread pinned to CPU(s): %s; only %u CPU(s) allowed, "
			      "main/worker/MQ threads may still share cores",
			      thread_name,
			      cpu_list,
			      cpu_count);
	} else {
		DOCA_LOG_INFO("%s thread pinned to CPU(s): %s", thread_name, cpu_list);
	}
}

void vnet_pci_device_pin_main_thread(void)
{
	vnet_pci_device_pin_current_thread(VNET_THREAD_AFFINITY_MAIN, "main TLP progress");
}

static struct vnet_pci_dev_controller *pci_cfg_workqueue_select_diag_controller(struct pci_cfg_workqueue *wq)
{
	struct tlp_context *tlp_ctx;
	struct vnet_pci_dev_controller *controller;
	struct vnet_pci_device *dev;
	struct vnet_virtio_common_config *pci_cfg;
	uint32_t attempt;
	uint32_t idx;

	if (wq == NULL || !wq->diag_collection_enabled || wq->diag_tlp_ctx == NULL)
		return NULL;

	tlp_ctx = wq->diag_tlp_ctx;
	if (tlp_ctx->num_ep == 0)
		return NULL;

	for (attempt = 0; attempt < tlp_ctx->num_ep; attempt++) {
		idx = wq->diag_next_controller;
		wq->diag_next_controller = (idx + 1) % tlp_ctx->num_ep;
		controller = &tlp_ctx->vnet_controller[idx];
		dev = atomic_load(&controller->virtio_device);

		if (dev == NULL)
			continue;
		if (!atomic_load(&controller->engine_enabled))
			continue;
		if (atomic_load(&controller->shutting_down) || atomic_load(&controller->stop_stats_collection) ||
		    atomic_load(&controller->cleanup_running) || atomic_load(&controller->initialization_in_progress) ||
		    atomic_load(&dev->cancel_in_progress)) {
			continue;
		}
		pci_cfg = vnet_pci_device_get_pci_cfg(dev);
		if (pci_cfg == NULL)
			continue;
		if (!(pci_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK))
			continue;

		return controller;
	}

	return NULL;
}

/************************************************************************
 ******                Global Variables & Resources                ******
 ************************************************************************/
/* struct to keep all enumerated devices */
static struct vnet_pci_device *vnet_pci_dev_space[256][32][8];
/* list of all enumerated devices, needed to find device by addr */
static struct vnet_pci_device_list vnet_pci_dev_enumerated_devs;

/* Forward declarations for VirtIO core functions */
static void vnet_pci_device_queue_init(struct vnet_pci_device *dev);
static doca_error_t vnet_pci_device_init_internal(struct vnet_pci_device *dev,
						  uint32_t index,
						  const struct vnet_pci_device_attrs *attr);
static void vnet_pci_device_pci_cfg_write32(struct vnet_pci_device *dev,
					    const uint64_t offset,
					    const uint32_t val,
					    const uint32_t wr_mask);
static uint64_t vnet_pci_dev_bar_offset(struct vnet_pci_device *dev, uint64_t addr);
static uint32_t vnet_pci_dev_feature_select(int n, uint64_t ftr);
static uint32_t vnet_pci_device_mmio_read32(struct vnet_pci_device *dev, const uint64_t addr);
static void vnet_pci_device_mmio_write32(struct vnet_pci_device *dev,
					 const uint64_t addr,
					 const uint32_t val,
					 const uint32_t wr_mask);
static void vnet_pci_device_reset(struct vnet_pci_device *dev);

/************************************************************************
 ******              Workqueue Implementation Functions            ******
 ************************************************************************/

/*
 * Allocate work item from pre-allocated pool (lock-free fast path)
 */
static struct pci_cfg_work_item *work_pool_alloc(void)
{
	uint32_t i;
	uint32_t free_before;

	if (pci_cfg_wq == NULL) {
		DOCA_LOG_ERR("work_pool_alloc: workqueue not initialized!");
		return NULL;
	}

	DOCA_LOG_DBG("work_pool_alloc: acquiring spinlock...");
	pthread_spin_lock(&pci_cfg_wq->pool.lock);
	free_before = pci_cfg_wq->pool.free_count;

	if (pci_cfg_wq->pool.free_count == 0) {
		pthread_spin_unlock(&pci_cfg_wq->pool.lock);
		DOCA_LOG_WARN("Work pool exhausted (size=%d, pending=%u) - work item dropped",
			      PCI_CFG_WORK_POOL_SIZE,
			      pci_cfg_wq->pending_work_count);
		return NULL;
	}

	/* Find first free slot */
	for (i = 0; i < PCI_CFG_WORK_POOL_SIZE; i++) {
		if (!pci_cfg_wq->pool.items[i].in_use) {
			memset(&pci_cfg_wq->pool.items[i], 0, sizeof(pci_cfg_wq->pool.items[i]));
			pci_cfg_wq->pool.items[i].in_use = true;
			pci_cfg_wq->pool.free_count--;
			DOCA_LOG_DBG("work_pool_alloc: allocated slot %u (free: %u -> %u)",
				     i,
				     free_before,
				     pci_cfg_wq->pool.free_count);
			pthread_spin_unlock(&pci_cfg_wq->pool.lock);
			return &pci_cfg_wq->pool.items[i];
		}
	}

	pthread_spin_unlock(&pci_cfg_wq->pool.lock);
	DOCA_LOG_ERR("Work pool inconsistency - free_count=%u but no free slot found", free_before);
	return NULL;
}

/*
 * Return work item to pool
 */
static void work_pool_free(struct pci_cfg_work_item *work)
{
	if (pci_cfg_wq == NULL || work == NULL)
		return;

	/* Verify work item belongs to pool */
	if (work < &pci_cfg_wq->pool.items[0] || work >= &pci_cfg_wq->pool.items[PCI_CFG_WORK_POOL_SIZE]) {
		DOCA_LOG_ERR("Invalid work item pointer - not from pool");
		return;
	}

	pthread_spin_lock(&pci_cfg_wq->pool.lock);
	work->in_use = false;
	pci_cfg_wq->pool.free_count++;
	pthread_spin_unlock(&pci_cfg_wq->pool.lock);
}

static uint32_t vnet_pci_device_get_reset_generation(const struct vnet_pci_device *dev)
{
	if (dev == NULL)
		return 0;

	return atomic_load(&dev->reset_generation);
}

static uint32_t vnet_controller_get_reset_generation(const struct vnet_pci_dev_controller *controller)
{
	struct vnet_pci_device *dev;

	if (controller == NULL)
		return 0;

	dev = atomic_load(&controller->virtio_device);
	return vnet_pci_device_get_reset_generation(dev);
}

static bool vnet_controller_get_valid_reset_generation(const struct vnet_pci_dev_controller *controller,
						       const char *work_name,
						       uint32_t *reset_generation)
{
	uint32_t generation;

	if (reset_generation == NULL)
		return false;

	*reset_generation = 0;
	if (controller == NULL) {
		DOCA_LOG_DBG("%s work dropped - controller is NULL", work_name);
		return false;
	}

	generation = vnet_controller_get_reset_generation(controller);
	if (generation == 0) {
		DOCA_LOG_DBG("%s work dropped - no active VirtIO device", work_name);
		return false;
	}

	*reset_generation = generation;
	return true;
}

static bool pci_cfg_work_item_is_stale(const struct pci_cfg_work_item *work)
{
	struct vnet_pci_dev_controller *controller = NULL;

	if (work == NULL || work->reset_generation == 0)
		return false;

	if (work->dev != NULL)
		return vnet_pci_device_get_reset_generation(work->dev) != work->reset_generation;

	switch (work->type) {
	case PCI_CFG_WORK_CONTROLLER_CLEANUP:
		controller = work->data.controller_cleanup.controller;
		break;
	case PCI_CFG_WORK_ENGINE_START:
		controller = work->data.engine_start.controller;
		break;
	case PCI_CFG_WORK_INITIALIZE_VQS:
		controller = work->data.initialize_vqs.controller;
		break;
	case PCI_CFG_WORK_INITIALIZE_IO_CTX:
		controller = work->data.initialize_io_ctx.controller;
		break;
	case PCI_CFG_WORK_CREATE_STATS_LIST:
		controller = work->data.create_stats_list.controller;
		break;
	case PCI_CFG_WORK_SPEED_CHANGE:
		controller = work->data.speed_change.controller;
		break;
	default:
		DOCA_LOG_WARN("Dropping work type %d - no reset generation owner", work->type);
		return true;
	}

	return vnet_controller_get_reset_generation(controller) != work->reset_generation;
}

/*
 * Worker thread function - processes PCI config operations from the queue
 */
static void *pci_cfg_worker_thread(void *arg)
{
	struct pci_cfg_workqueue *wq = (struct pci_cfg_workqueue *)arg;
	struct vnet_pci_dev_controller *diag_controller = NULL;
	struct pci_cfg_work_item *work;
	bool collect_diagnostics;

	vnet_pci_device_pin_current_thread(VNET_THREAD_AFFINITY_WORKER, "PCI config worker");
	DOCA_LOG_INFO("PCI config workqueue worker thread started (tid=%ld)", pthread_self());

	while (1) {
		struct timespec timeout;
		struct doca_pe *pes_to_progress[MAX_NUM_EP];
		uint32_t pe_count_to_progress;
		uint32_t i;
		int wait_result;

		pthread_mutex_lock(&wq->mutex);

		/* Wait for work or shutdown signal with timeout.
		 * Use timed wait to ensure PE2 gets progressed regularly even when idle.
		 * This prevents IO context events (CVQ ctrl_req) from stalling. */
		while (wq->head == NULL && !wq->shutdown) {
			clock_gettime(CLOCK_REALTIME, &timeout);
			timeout.tv_nsec += 10 * 1000000; /* 10ms timeout */
			if (timeout.tv_nsec >= 1000000000) {
				timeout.tv_sec += 1;
				timeout.tv_nsec -= 1000000000;
			}
			wait_result = pthread_cond_timedwait(&wq->cond, &wq->mutex, &timeout);

			/* On timeout, progress ALL worker PEs to handle IO context events.
			 * Multi-endpoint mode has per-controller worker PEs - we must progress
			 * all of them to avoid stalling controllers 1..N.
			 * Save local copies to avoid race with clear_worker_pes(). */
			if (wait_result == ETIMEDOUT && !wq->shutdown) {
				pe_count_to_progress = wq->worker_pe_count;
				for (i = 0; i < pe_count_to_progress; i++)
					pes_to_progress[i] = wq->worker_pes[i];
				diag_controller = NULL;
				if (!wq->shutdown && wq->head == NULL && !wq->reset_in_progress)
					diag_controller = pci_cfg_workqueue_select_diag_controller(wq);
				if (pe_count_to_progress > 0 || diag_controller != NULL) {
					pthread_mutex_unlock(&wq->mutex);
					for (i = 0; i < pe_count_to_progress; i++) {
						if (pes_to_progress[i] != NULL)
							(void)doca_pe_progress(pes_to_progress[i]);
					}
					if (diag_controller != NULL) {
						pthread_mutex_lock(&wq->mutex);
						collect_diagnostics = !wq->shutdown && wq->head == NULL &&
								      !wq->reset_in_progress;
						pthread_mutex_unlock(&wq->mutex);
						if (collect_diagnostics)
							vnet_pci_dev_worker_collect_diagnostics(diag_controller);
					}
					pthread_mutex_lock(&wq->mutex);
					diag_controller = NULL;
				}
			}
		}

		/* Check for shutdown */
		if (wq->shutdown && wq->head == NULL) {
			pthread_mutex_unlock(&wq->mutex);
			break;
		}

		/* Dequeue work item */
		work = wq->head;
		if (work != NULL) {
			wq->head = work->next;
			if (wq->head == NULL)
				wq->tail = NULL;
			wq->pending_work_count--;
			DOCA_LOG_DBG("Worker: dequeued work type %d (pending=%u, pool_free=%u)",
				     work->type,
				     wq->pending_work_count,
				     wq->pool.free_count);
		}

		pthread_mutex_unlock(&wq->mutex);

		/* Execute work outside the lock */
		if (work != NULL) {
			struct doca_pe *pes_snapshot[MAX_NUM_EP];
			uint32_t pe_count_snapshot;
			doca_error_t result;
			uint32_t pe_idx;
			int pe_iterations;

			/* Reset barrier: wait if reset is in progress.
			 * This ensures reset completes before we access dev->vqs.
			 * Reset path waits for worker_busy=false (same device only),
			 * we wait for reset_in_progress=false before proceeding. */
			pthread_mutex_lock(&wq->mutex);
			while (wq->reset_in_progress && !wq->shutdown) {
				DOCA_LOG_DBG("Worker waiting for reset to complete before processing work type %d",
					     work->type);
				pthread_cond_wait(&wq->reset_cond, &wq->mutex);
			}
			if (wq->shutdown) {
				pthread_mutex_unlock(&wq->mutex);
				work_pool_free(work);
				continue;
			}
			wq->worker_busy = true;
			wq->worker_current_dev = work->dev;
			pthread_mutex_unlock(&wq->mutex);

			if (pci_cfg_work_item_is_stale(work)) {
				DOCA_LOG_DBG("Skipping stale work type %d (gen=%u)",
					     work->type,
					     work->reset_generation);
				pthread_mutex_lock(&wq->mutex);
				goto work_done;
			}

			switch (work->type) {
			case PCI_CFG_WORK_VQ_CONFIG:
				DOCA_LOG_INFO("Executing VQ%d config", work->data.vq_config.vq_index);
				vnet_pci_device_vq_config(work->dev, work->data.vq_config.vq_index);
				pthread_mutex_lock(&wq->mutex);
				wq->vq_config_count++;
				break;

			case PCI_CFG_WORK_CONTROLLER_CLEANUP:
				DOCA_LOG_INFO("Executing controller cleanup (destroy_engine=%d)",
					      work->data.controller_cleanup.destroy_engine);
				result = vnet_controller_cleanup(work->data.controller_cleanup.controller,
								 work->data.controller_cleanup.destroy_engine);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Controller cleanup failed: %s", doca_error_get_name(result));
				pthread_mutex_lock(&wq->mutex);
				wq->controller_cleanup_count++;
				break;

			case PCI_CFG_WORK_ENGINE_START:
				DOCA_LOG_INFO("Executing offload engine start");
				{
					struct doca_devemu_virtio_offload_engine *virtio_engine;
					if (!work->data.engine_start.controller->offload_engine ||
					    atomic_load(&work->data.engine_start.controller->cleanup_running)) {
						DOCA_LOG_WARN("Offload engine destroyed or cleanup in progress"
							      " - skipping engine start");
						pthread_mutex_lock(&wq->mutex);
						wq->engine_start_count++;
						break;
					}
					virtio_engine = doca_devemu_vnet_offload_engine_as_virtio_offload(
						work->data.engine_start.controller->offload_engine);
					if (virtio_engine) {
						result = doca_devemu_virtio_offload_engine_start(virtio_engine);
						if (result == DOCA_SUCCESS) {
							atomic_store(&work->data.engine_start.controller
									      ->offload_engine_started,
								     true);
							DOCA_LOG_INFO("VNet offload engine started (async)");
						} else {
							DOCA_LOG_ERR("Failed to start offload engine: %s",
								     doca_error_get_name(result));
						}
					}
				}
				pthread_mutex_lock(&wq->mutex);
				wq->engine_start_count++;
				break;

			case PCI_CFG_WORK_INITIALIZE_VQS:
				DOCA_LOG_INFO("Executing VQs initialization");
				result = vnet_pci_dev_initialize_vqs(work->data.initialize_vqs.controller);
				if (result == DOCA_ERROR_BAD_STATE)
					DOCA_LOG_DBG("VQs init skipped (stale work item)");
				else if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Failed to initialize VQs: %s", doca_error_get_name(result));
				else
					DOCA_LOG_INFO("VQ structures created (async)");
				pthread_mutex_lock(&wq->mutex);
				wq->initialize_vqs_count++;
				break;

			case PCI_CFG_WORK_INITIALIZE_IO_CTX:
				DOCA_LOG_INFO("Executing IO context initialization");
				result = vnet_pci_dev_initialize_io_context(work->data.initialize_io_ctx.controller);
				if (result == DOCA_ERROR_BAD_STATE)
					DOCA_LOG_DBG("IO context init skipped (stale work item)");
				else if (result != DOCA_SUCCESS)
					DOCA_LOG_WARN("IO context initialization failed: %s",
						      doca_error_get_name(result));
				pthread_mutex_lock(&wq->mutex);
				wq->initialize_io_ctx_count++;
				break;

			case PCI_CFG_WORK_START_AND_ENABLE: {
				struct vnet_pci_dev_controller *ctrl = work->data.start_and_enable.controller;

				/* Do NOT skip start_and_enable when pending_unplug is set.
				 * The host driver is already past DRIVER_OK and may be
				 * issuing CVQ commands (e.g. _virtnet_set_queues).  If the
				 * engine is never enabled, virtnet_send_command() spins
				 * forever waiting for CVQ completion, causing an RCU stall
				 * that locks up the entire host.  The device will be torn
				 * down later when the host completes the ABP unplug
				 * sequence (power-off or timeout-forced destroy). */
				if (ctrl->tlp_ctx && work->dev && work->dev->pf_index >= 0 &&
				    (uint32_t)work->dev->pf_index < ctrl->tlp_ctx->num_ep) {
					struct pci_device_config *ep =
						&ctrl->tlp_ctx->devs_config[FIRST_PF_IDX(ctrl->tlp_ctx) +
									    work->dev->pf_index];
					if (atomic_load(&ep->pending_unplug))
						DOCA_LOG_INFO("start_and_enable with pending_unplug on DSP[%d] - "
							      "proceeding (host driver needs working engine)",
							      work->dev->pf_index);
				}
				DOCA_LOG_INFO("Executing VQs start and engine enable");
				result = vnet_pci_dev_start_and_enable(work->data.start_and_enable.controller,
								       work->dev);
				if (result == DOCA_ERROR_BAD_STATE)
					DOCA_LOG_DBG("start_and_enable skipped (stale work item)");
				else if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Failed to start VQs and enable engine: %s",
						     doca_error_get_name(result));
				pthread_mutex_lock(&wq->mutex);
				wq->start_and_enable_count++;
				break;
			}

			case PCI_CFG_WORK_CREATE_STATS_LIST:
				DOCA_LOG_INFO("Executing stats list creation");
				{
					struct vnet_pci_dev_controller *ctrl = work->data.create_stats_list.controller;
					struct doca_devemu_virtio_offload_engine *virtio_oe;
					struct vnet_stats_list_ref *old_ref;
					struct doca_devemu_virtio_queue_dbg_state **temp_state_list = NULL;
					uint32_t temp_list_len = 0;

					/* Check stop flags BEFORE slow DOCA call - abort early if shutting down */
					if (atomic_load(&ctrl->stop_stats_collection) ||
					    atomic_load(&ctrl->shutting_down)) {
						DOCA_LOG_DBG("Stats list creation aborted - shutdown in progress");
						goto stats_list_done;
					}

					/* Skip if stats collection is in progress.
					 * DOCA may still be writing to the current state_list via async populate.
					 * Swapping and freeing the old list now would cause UAF. */
					if (atomic_load(&ctrl->dbg_state.stats_in_progress)) {
						DOCA_LOG_DBG(
							"Stats list creation skipped - stats collection in progress");
						goto stats_list_done;
					}

					/* Check offload_engine before conversion - can be NULL during teardown */
					if (ctrl->offload_engine == NULL) {
						DOCA_LOG_DBG("Stats list creation skipped - offload_engine is NULL");
						goto stats_list_done;
					}

					/* Skip if the engine is not enabled (e.g. start_and_enable was skipped due to
					 * reset) */
					if (!atomic_load(&ctrl->engine_enabled)) {
						DOCA_LOG_DBG("Stats list creation skipped - engine is not enabled");
						goto stats_list_done;
					}

					virtio_oe =
						doca_devemu_vnet_offload_engine_as_virtio_offload(ctrl->offload_engine);
					if (virtio_oe == NULL) {
						DOCA_LOG_ERR("Failed to get virtio offload engine");
						goto stats_list_done;
					}

					/* Create new stats list BEFORE taking mutex (slow DOCA API).
					 * This prevents cleanup from hanging on mutex_lock. */
					result = doca_devemu_virtio_offload_engine_queue_dbg_state_create_list(
						virtio_oe,
						&temp_state_list,
						&temp_list_len);
					if (result != DOCA_SUCCESS) {
						DOCA_LOG_ERR("Failed to create stats list: %s",
							     doca_error_get_name(result));
						goto stats_list_done;
					}

					/* Allocate new reference struct before taking mutex */
					struct vnet_stats_list_ref *new_ref =
						malloc(sizeof(struct vnet_stats_list_ref));
					if (new_ref == NULL) {
						DOCA_LOG_ERR("Failed to allocate stats_ref");
						(void)doca_devemu_virtio_queue_dbg_state_destroy_list(temp_state_list);
						goto stats_list_done;
					}
					new_ref->state_list = temp_state_list;
					new_ref->list_len = temp_list_len;

					pthread_mutex_lock(&ctrl->stats_ref_mutex);

					/* Re-check stop flags and stats_in_progress after acquiring mutex.
					 * stats_in_progress may have become true while we were creating
					 * the new list (without the mutex held). If DOCA is currently
					 * using old state_list via async populate, freeing it would UAF. */
					if (atomic_load(&ctrl->stop_stats_collection) ||
					    atomic_load(&ctrl->shutting_down) ||
					    atomic_load(&ctrl->dbg_state.stats_in_progress)) {
						pthread_mutex_unlock(&ctrl->stats_ref_mutex);
						DOCA_LOG_DBG("Stats list creation aborted after lock "
							     "(stop=%d, shutdown=%d, in_progress=%d)",
							     atomic_load(&ctrl->stop_stats_collection),
							     atomic_load(&ctrl->shutting_down),
							     atomic_load(&ctrl->dbg_state.stats_in_progress));
						(void)doca_devemu_virtio_queue_dbg_state_destroy_list(temp_state_list);
						free(new_ref);
						goto stats_list_done;
					}

					/* Swap: get old, publish new */
					old_ref = atomic_load(&ctrl->stats_ref);
					atomic_store(&ctrl->stats_ref, new_ref);

					pthread_mutex_unlock(&ctrl->stats_ref_mutex);

					/* Destroy old list outside mutex.
					 * Safe because: stats_in_progress was false when we swapped (checked
					 * above under mutex), so no async populate is using old state_list. */
					if (old_ref != NULL) {
						DOCA_LOG_DBG("Destroying old stats list");
						(void)doca_devemu_virtio_queue_dbg_state_destroy_list(
							old_ref->state_list);
						free(old_ref);
					}

					DOCA_LOG_INFO("Created stats list for %u queues (async)", temp_list_len);
				}
stats_list_done:
				pthread_mutex_lock(&wq->mutex);
				wq->create_stats_list_count++;
				break;

			case PCI_CFG_WORK_DEVICE_RESET:
				DOCA_LOG_INFO("Executing device reset");
				vnet_pci_device_reset(work->dev);
				pthread_mutex_lock(&wq->mutex);
				wq->device_reset_count++;
				break;

			case PCI_CFG_WORK_DELAYED_DESTROY:
				/*
				 * Delayed device destruction for hotplug removal.
				 * This is triggered after host driver reset (device_status=0).
				 * Safe to destroy the device now.
				 */
				{
					struct tlp_context *tlp_ctx = work->data.delayed_destroy.tlp_ctx;
					struct pci_device_config *endpoint = work->data.delayed_destroy.endpoint;
					doca_error_t result;

					DOCA_LOG_INFO("Executing delayed destroy after host reset");
					result = vnet_pci_dev_destroy_device(tlp_ctx, endpoint);
					if (result != DOCA_SUCCESS) {
						DOCA_LOG_ERR("Delayed destroy failed: %s",
							     doca_error_get_descr(result));
						/* Restore flags to allow retry on next power-off write.
						 * Clear pending_destroy to allow re-submission. */
						atomic_store(&endpoint->pending_destroy, false);
						/* Keep pending_unplug true to allow retry */
					} else {
						DOCA_LOG_INFO("Device destroyed successfully (delayed)");
						/* Clear both flags on success */
						atomic_store(&endpoint->pending_unplug, false);
						atomic_store(&endpoint->pending_destroy, false);
					}
				}
				pthread_mutex_lock(&wq->mutex);
				wq->delayed_destroy_count++;
				break;

			case PCI_CFG_WORK_HOTPLUG_EVENT:
				/*
				 * Hotplug device creation/removal on worker thread.
				 * CRITICAL: create_device() takes 350-400ms of blocking DOCA API calls.
				 * Running it on the main thread starves doca_pe_progress(), causing
				 * PCIe Completion Timeouts when the host sends config TLPs.
				 */
				{
					struct tlp_context *ctx = work->data.hotplug_event.tlp_ctx;
					uint32_t idx = work->data.hotplug_event.dsp_index;
					bool is_plug = work->data.hotplug_event.plug;

					DOCA_LOG_INFO("Executing async hotplug: DSP[%u] %s",
						      idx,
						      is_plug ? "plug" : "unplug");
					result = vnet_pci_dev_execute_hotplug(ctx, idx, is_plug);
					if (result == DOCA_ERROR_AGAIN)
						DOCA_LOG_WARN("Async hotplug DSP[%u] %s: transient, retry later",
							      idx,
							      is_plug ? "plug" : "unplug");
					else if (result != DOCA_SUCCESS)
						DOCA_LOG_ERR("Async hotplug failed: %s", doca_error_get_descr(result));
					else
						DOCA_LOG_INFO("Async hotplug DSP[%u] %s completed",
							      idx,
							      is_plug ? "plug" : "unplug");
				}
				pthread_mutex_lock(&wq->mutex);
				break;

			case PCI_CFG_WORK_SPEED_CHANGE:
				result = vnet_pci_dev_execute_speed_change(work->data.speed_change.controller,
									   work->data.speed_change.new_speed);
				if (result != DOCA_SUCCESS)
					DOCA_LOG_ERR("Speed change failed: %s", doca_error_get_descr(result));
				pthread_mutex_lock(&wq->mutex);
				break;

			default:
				DOCA_LOG_ERR("Unknown work type: %d", work->type);
				pthread_mutex_lock(&wq->mutex);
				break;
			}

work_done:
			/* Save worker PEs snapshot and release mutex before PE progress loop.
			 * This reduces mutex hold time and allows enqueues during PE progress. */
			pe_count_snapshot = (!wq->shutdown) ? wq->worker_pe_count : 0;
			for (pe_idx = 0; pe_idx < pe_count_snapshot; pe_idx++)
				pes_snapshot[pe_idx] = wq->worker_pes[pe_idx];
			wq->total_processed++;
			DOCA_LOG_DBG("Worker: completed work type %d (total=%lu, pool_free=%u)",
				     work->type,
				     wq->total_processed,
				     wq->pool.free_count);
			pthread_mutex_unlock(&wq->mutex);

			/* Drive ALL worker PEs (PE2) to complete async DOCA operations.
			 * Multi-endpoint mode has per-controller worker PEs - we must
			 * progress all of them to avoid stalling controllers 1..N.
			 * PE progress runs outside mutex to minimize lock hold time. */
			for (pe_idx = 0; pe_idx < pe_count_snapshot; pe_idx++) {
				if (pes_snapshot[pe_idx] != NULL) {
					for (pe_iterations = 0; pe_iterations < 100; pe_iterations++)
						(void)doca_pe_progress(pes_snapshot[pe_idx]);
				}
			}

			/* Mark worker as idle and signal reset waiter if any.
			 * This completes the per-device reset barrier handshake.
			 * Clear worker_current_dev so the per-device barrier check
			 * sees no device association when worker_busy transitions. */
			pthread_mutex_lock(&wq->mutex);
			wq->worker_busy = false;
			wq->worker_current_dev = NULL;
			pthread_cond_signal(&wq->reset_cond);
			pthread_mutex_unlock(&wq->mutex);

			/* Return work item to pool */
			work_pool_free(work);
		}
	}

	DOCA_LOG_INFO("PCI config workqueue worker thread exiting (processed %lu, dropped %lu)",
		      wq->total_processed,
		      wq->dropped_count);
	DOCA_LOG_DBG("  VQ configs: %lu, cleanups: %lu, engine starts: %lu, resets: %lu",
		     wq->vq_config_count,
		     wq->controller_cleanup_count,
		     wq->engine_start_count,
		     wq->device_reset_count);
	DOCA_LOG_DBG("  Init VQs: %lu, Init IO: %lu, Start/Enable: %lu, Stats: %lu",
		     wq->initialize_vqs_count,
		     wq->initialize_io_ctx_count,
		     wq->start_and_enable_count,
		     wq->create_stats_list_count);
	return NULL;
}

/*
 * Initialize the workqueue
 */
static doca_error_t pci_cfg_workqueue_init(void)
{
	int ret;
	uint32_t i;

	if (pci_cfg_wq != NULL) {
		DOCA_LOG_WARN("PCI config workqueue already initialized");
		return DOCA_SUCCESS;
	}

	pci_cfg_wq = calloc(1, sizeof(struct pci_cfg_workqueue));
	if (pci_cfg_wq == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for workqueue");
		return DOCA_ERROR_NO_MEMORY;
	}

	/* Initialize mutex and condition variable */
	ret = pthread_mutex_init(&pci_cfg_wq->mutex, NULL);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to initialize workqueue mutex: %d", ret);
		free(pci_cfg_wq);
		pci_cfg_wq = NULL;
		return DOCA_ERROR_INITIALIZATION;
	}

	ret = pthread_cond_init(&pci_cfg_wq->cond, NULL);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to initialize workqueue condition variable: %d", ret);
		pthread_mutex_destroy(&pci_cfg_wq->mutex);
		free(pci_cfg_wq);
		pci_cfg_wq = NULL;
		return DOCA_ERROR_INITIALIZATION;
	}

	ret = pthread_cond_init(&pci_cfg_wq->reset_cond, NULL);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to initialize reset condition variable: %d", ret);
		pthread_cond_destroy(&pci_cfg_wq->cond);
		pthread_mutex_destroy(&pci_cfg_wq->mutex);
		free(pci_cfg_wq);
		pci_cfg_wq = NULL;
		return DOCA_ERROR_INITIALIZATION;
	}

	/* Initialize work item pool */
	ret = pthread_spin_init(&pci_cfg_wq->pool.lock, PTHREAD_PROCESS_PRIVATE);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to initialize pool spinlock: %d", ret);
		pthread_cond_destroy(&pci_cfg_wq->reset_cond);
		pthread_cond_destroy(&pci_cfg_wq->cond);
		pthread_mutex_destroy(&pci_cfg_wq->mutex);
		free(pci_cfg_wq);
		pci_cfg_wq = NULL;
		return DOCA_ERROR_INITIALIZATION;
	}

	for (i = 0; i < PCI_CFG_WORK_POOL_SIZE; i++)
		pci_cfg_wq->pool.items[i].in_use = false;
	pci_cfg_wq->pool.free_count = PCI_CFG_WORK_POOL_SIZE;

	/* Initialize MQ start concurrency limiter semaphore.
	 * Each start_additional_queue_pairs for 127 QPs is very memory-intensive.
	 * Limit concurrent threads to MQ_START_MAX_CONCURRENT to prevent OOM. */
	ret = sem_init(&pci_cfg_wq->mq_start_sem, 0, MQ_START_MAX_CONCURRENT);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to initialize MQ start semaphore: %d", errno);
		pthread_spin_destroy(&pci_cfg_wq->pool.lock);
		pthread_cond_destroy(&pci_cfg_wq->reset_cond);
		pthread_cond_destroy(&pci_cfg_wq->cond);
		pthread_mutex_destroy(&pci_cfg_wq->mutex);
		free(pci_cfg_wq);
		pci_cfg_wq = NULL;
		return DOCA_ERROR_INITIALIZATION;
	}

	/* Initialize queue state */
	pci_cfg_wq->head = NULL;
	pci_cfg_wq->tail = NULL;
	pci_cfg_wq->running = true;
	pci_cfg_wq->shutdown = false;
	pci_cfg_wq->mq_start_teardown = false;
	pci_cfg_wq->pending_work_count = 0;
	pci_cfg_wq->total_processed = 0;
	pci_cfg_wq->dropped_count = 0;
	pci_cfg_wq->mq_threads = NULL;
	pci_cfg_wq->mq_thread_count = 0;

	/* Create worker thread */
	ret = pthread_create(&pci_cfg_wq->worker_thread, NULL, pci_cfg_worker_thread, pci_cfg_wq);
	if (ret != 0) {
		DOCA_LOG_ERR("Failed to create workqueue worker thread: %d", ret);
		sem_destroy(&pci_cfg_wq->mq_start_sem);
		pthread_spin_destroy(&pci_cfg_wq->pool.lock);
		pthread_cond_destroy(&pci_cfg_wq->reset_cond);
		pthread_cond_destroy(&pci_cfg_wq->cond);
		pthread_mutex_destroy(&pci_cfg_wq->mutex);
		free(pci_cfg_wq);
		pci_cfg_wq = NULL;
		return DOCA_ERROR_INITIALIZATION;
	}

	/* Set thread name for easier debugging */
	pthread_setname_np(pci_cfg_wq->worker_thread, "pci_cfg_worker");

	DOCA_LOG_INFO("PCI config workqueue initialized (pool size: %d)", PCI_CFG_WORK_POOL_SIZE);
	return DOCA_SUCCESS;
}

static void pci_cfg_workqueue_reap_completed_mq_threads(struct pci_cfg_workqueue *wq)
{
	struct mq_start_thread_node *join_list = NULL;
	struct mq_start_thread_node **cursor;
	struct mq_start_thread_node *node;
	uint32_t joined_count = 0;

	pthread_mutex_lock(&wq->mutex);
	cursor = &wq->mq_threads;
	while (*cursor != NULL) {
		node = *cursor;
		if (!node->completed) {
			cursor = &node->next;
			continue;
		}
		*cursor = node->next;
		wq->mq_thread_count--;
		node->next = join_list;
		join_list = node;
	}
	pthread_mutex_unlock(&wq->mutex);

	while (join_list != NULL) {
		node = join_list;
		join_list = node->next;
		pthread_join(node->thread, NULL);
		free(node);
		joined_count++;
	}

	if (joined_count > 0)
		DOCA_LOG_DBG("Reaped %u completed MQ start thread(s)", joined_count);
}

static void pci_cfg_workqueue_join_mq_threads(struct pci_cfg_workqueue *wq)
{
	struct mq_start_thread_node *node;
	uint32_t joined_count = 0;

	while (true) {
		pthread_mutex_lock(&wq->mutex);
		node = wq->mq_threads;
		if (node == NULL) {
			pthread_mutex_unlock(&wq->mutex);
			break;
		}
		wq->mq_threads = node->next;
		wq->mq_thread_count--;
		pthread_mutex_unlock(&wq->mutex);

		pthread_join(node->thread, NULL);
		free(node);
		joined_count++;
	}

	if (joined_count > 0)
		DOCA_LOG_INFO("Joined %u MQ start thread(s)", joined_count);
}

/*
 * Shutdown the workqueue - stop accepting new work and wait for pending work to complete.
 * This must be called before destroying any resources that work items may reference.
 */
void pci_cfg_workqueue_shutdown(void)
{
	uint32_t pending_count;
	uint64_t processed_count, dropped_count;

	if (pci_cfg_wq == NULL)
		return;

	/* Check and set shutdown flag under lock to avoid data race */
	pthread_mutex_lock(&pci_cfg_wq->mutex);
	if (pci_cfg_wq->shutdown) {
		pthread_mutex_unlock(&pci_cfg_wq->mutex);
		return;
	}
	pending_count = pci_cfg_wq->pending_work_count;
	processed_count = pci_cfg_wq->total_processed;
	pci_cfg_wq->shutdown = true;
	/* Wake worker thread regardless of which condition it's waiting on.
	 * Worker may be blocked on cond (waiting for work) or reset_cond (reset barrier).
	 * Must wake both to avoid deadlock on shutdown. */
	pthread_cond_signal(&pci_cfg_wq->cond);
	pthread_cond_broadcast(&pci_cfg_wq->reset_cond);
	pthread_mutex_unlock(&pci_cfg_wq->mutex);

	DOCA_LOG_INFO("Shutting down PCI config workqueue (pending: %u, processed: %lu)",
		      pending_count,
		      processed_count);

	/* Wait for worker thread to finish all pending work and exit */
	pthread_join(pci_cfg_wq->worker_thread, NULL);
	pci_cfg_wq->running = false;

	pci_cfg_workqueue_join_mq_threads(pci_cfg_wq);

	/* Read final counts under lock */
	pthread_mutex_lock(&pci_cfg_wq->mutex);
	processed_count = pci_cfg_wq->total_processed;
	dropped_count = pci_cfg_wq->dropped_count;
	pthread_mutex_unlock(&pci_cfg_wq->mutex);

	DOCA_LOG_INFO("PCI config workqueue shutdown complete (processed: %lu, dropped: %lu)",
		      processed_count,
		      dropped_count);
}

/*
 * Destroy the workqueue - free all resources.
 * pci_cfg_workqueue_shutdown() should be called first.
 */
static void pci_cfg_workqueue_destroy(void)
{
	struct pci_cfg_workqueue *wq = pci_cfg_wq;
	struct pci_cfg_work_item *work, *next;
	uint32_t dropped = 0;
	uint32_t i;

	if (wq == NULL)
		return;

	/* Ensure shutdown is complete (this handles thread join) */
	if (!wq->shutdown)
		pci_cfg_workqueue_shutdown();

	/* Clean up any remaining work items in queue (should be empty after shutdown) */
	pthread_mutex_lock(&wq->mutex);
	work = wq->head;
	while (work != NULL) {
		next = work->next;
		DOCA_LOG_WARN("Dropping unprocessed work item (type: %d)", work->type);
		work_pool_free(work);
		dropped++;
		work = next;
	}
	wq->mq_start_teardown = true;
	pthread_mutex_unlock(&wq->mutex);

	if (dropped > 0)
		DOCA_LOG_WARN("Dropped %u unprocessed work items during shutdown", dropped);

	for (i = 0; i < MQ_START_MAX_CONCURRENT; i++)
		sem_post(&wq->mq_start_sem);
	while (atomic_load(&mq_start_threads_active) != 0)
		usleep(1000);

	/* Destroy synchronization primitives */
	sem_destroy(&wq->mq_start_sem);
	pthread_spin_destroy(&wq->pool.lock);
	pthread_cond_destroy(&wq->reset_cond);
	pthread_cond_destroy(&wq->cond);
	pthread_mutex_destroy(&wq->mutex);

	/* Free workqueue structure */
	free(wq);
	pci_cfg_wq = NULL;

	DOCA_LOG_INFO("PCI config workqueue destroyed");
}

/*
 * Helper function to enqueue work item (fire and forget)
 */
static void pci_cfg_workqueue_enqueue(struct pci_cfg_work_item *work)
{
	if (work == NULL)
		return;

	DOCA_LOG_DBG("workqueue_enqueue: acquiring mutex for work type %d...", work->type);
	pthread_mutex_lock(&pci_cfg_wq->mutex);
	DOCA_LOG_DBG("workqueue_enqueue: mutex acquired");

	/* Silently drop during shutdown - this is expected behavior */
	if (pci_cfg_wq->shutdown) {
		pci_cfg_wq->dropped_count++;
		DOCA_LOG_DBG("workqueue_enqueue: dropping work due to shutdown");
		pthread_mutex_unlock(&pci_cfg_wq->mutex);
		work_pool_free(work);
		return;
	}

	/* Enqueue work item */
	work->next = NULL;
	if (pci_cfg_wq->tail != NULL) {
		pci_cfg_wq->tail->next = work;
		pci_cfg_wq->tail = work;
	} else {
		pci_cfg_wq->head = work;
		pci_cfg_wq->tail = work;
	}

	pci_cfg_wq->pending_work_count++;
	DOCA_LOG_DBG("workqueue_enqueue: enqueued work type %d (pending=%u, pool_free=%u)",
		     work->type,
		     pci_cfg_wq->pending_work_count,
		     pci_cfg_wq->pool.free_count);
	pthread_cond_signal(&pci_cfg_wq->cond);
	pthread_mutex_unlock(&pci_cfg_wq->mutex);
}

void pci_cfg_workqueue_submit_controller_cleanup(struct vnet_pci_dev_controller *controller, bool destroy_engine)
{
	struct pci_cfg_work_item *work;
	uint32_t reset_generation;

	if (!vnet_controller_get_valid_reset_generation(controller, "Controller cleanup", &reset_generation))
		return;

	work = work_pool_alloc();
	if (work == NULL)
		return; /* Pool exhausted - logged internally */

	work->type = PCI_CFG_WORK_CONTROLLER_CLEANUP;
	work->dev = NULL;
	work->data.controller_cleanup.controller = controller;
	work->data.controller_cleanup.destroy_engine = destroy_engine;
	/* Cleanup belongs to the reset generation that queued it; newer resets must not let old cleanup
	 * destroy resources created by a later probe cycle. */
	work->reset_generation = reset_generation;

	DOCA_LOG_DBG("Controller cleanup queued (destroy_engine=%d)", destroy_engine);
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_submit_engine_start(struct vnet_pci_dev_controller *controller)
{
	struct pci_cfg_work_item *work;
	uint32_t reset_generation;

	if (!vnet_controller_get_valid_reset_generation(controller, "Engine start", &reset_generation))
		return;

	work = work_pool_alloc();
	if (work == NULL)
		return;

	work->type = PCI_CFG_WORK_ENGINE_START;
	work->dev = NULL;
	work->data.engine_start.controller = controller;
	work->reset_generation = reset_generation;

	DOCA_LOG_DBG("Engine start queued");
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_submit_initialize_vqs(struct vnet_pci_dev_controller *controller)
{
	struct pci_cfg_work_item *work;
	uint32_t reset_generation;

	if (!vnet_controller_get_valid_reset_generation(controller, "Initialize VQs", &reset_generation))
		return;

	work = work_pool_alloc();
	if (work == NULL)
		return;

	work->type = PCI_CFG_WORK_INITIALIZE_VQS;
	work->dev = NULL;
	work->data.initialize_vqs.controller = controller;
	work->reset_generation = reset_generation;

	DOCA_LOG_DBG("Initialize VQs queued");
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_submit_initialize_io_context(struct vnet_pci_dev_controller *controller)
{
	struct pci_cfg_work_item *work;
	uint32_t reset_generation;

	if (!vnet_controller_get_valid_reset_generation(controller, "Initialize IO context", &reset_generation))
		return;

	work = work_pool_alloc();
	if (work == NULL)
		return;

	work->type = PCI_CFG_WORK_INITIALIZE_IO_CTX;
	work->dev = NULL;
	work->data.initialize_io_ctx.controller = controller;
	work->reset_generation = reset_generation;

	DOCA_LOG_DBG("Initialize IO context queued");
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_submit_start_and_enable(struct vnet_pci_dev_controller *controller, struct vnet_pci_device *dev)
{
	struct vnet_pci_device *active_dev;
	struct pci_cfg_work_item *work;
	uint32_t reset_generation;

	if (!vnet_controller_get_valid_reset_generation(controller, "Start and enable", &reset_generation))
		return;
	active_dev = atomic_load(&controller->virtio_device);
	if (active_dev != dev) {
		DOCA_LOG_DBG("Start and enable work dropped - device is not active");
		return;
	}

	work = work_pool_alloc();
	if (work == NULL)
		return;

	work->type = PCI_CFG_WORK_START_AND_ENABLE;
	work->dev = dev;
	work->data.start_and_enable.controller = controller;
	work->reset_generation = reset_generation;

	DOCA_LOG_DBG("Start and enable queued");
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_submit_create_stats_list(struct vnet_pci_dev_controller *controller)
{
	struct pci_cfg_work_item *work;
	uint32_t reset_generation;

	if (!vnet_controller_get_valid_reset_generation(controller, "Create stats list", &reset_generation))
		return;

	work = work_pool_alloc();
	if (work == NULL)
		return;

	work->type = PCI_CFG_WORK_CREATE_STATS_LIST;
	work->dev = NULL;
	work->data.create_stats_list.controller = controller;
	work->reset_generation = reset_generation;

	DOCA_LOG_DBG("Create stats list queued");
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_log_status(void)
{
	if (pci_cfg_wq == NULL) {
		DOCA_LOG_WARN("Workqueue not initialized");
		return;
	}

	pthread_spin_lock(&pci_cfg_wq->pool.lock);
	uint32_t pool_free = pci_cfg_wq->pool.free_count;
	pthread_spin_unlock(&pci_cfg_wq->pool.lock);

	pthread_mutex_lock(&pci_cfg_wq->mutex);
	uint32_t pending = pci_cfg_wq->pending_work_count;
	uint64_t total_processed = pci_cfg_wq->total_processed;
	uint64_t dropped = pci_cfg_wq->dropped_count;
	bool shutdown = pci_cfg_wq->shutdown;
	pthread_mutex_unlock(&pci_cfg_wq->mutex);

	DOCA_LOG_INFO("Workqueue status: pool_free=%u/%d, pending=%u, processed=%lu, dropped=%lu, shutdown=%d",
		      pool_free,
		      PCI_CFG_WORK_POOL_SIZE,
		      pending,
		      total_processed,
		      dropped,
		      shutdown);
}

void pci_cfg_workqueue_set_worker_pes(struct doca_pe **pes, uint32_t count)
{
	uint32_t i;

	if (pci_cfg_wq == NULL) {
		DOCA_LOG_WARN("Workqueue not initialized - cannot set worker PEs");
		return;
	}

	if (count > MAX_NUM_EP) {
		DOCA_LOG_ERR("Worker PE count %u exceeds maximum %u", count, MAX_NUM_EP);
		count = MAX_NUM_EP;
	}

	pthread_mutex_lock(&pci_cfg_wq->mutex);
	for (i = 0; i < count; i++)
		pci_cfg_wq->worker_pes[i] = pes[i];
	pci_cfg_wq->worker_pe_count = count;
	pthread_mutex_unlock(&pci_cfg_wq->mutex);
	DOCA_LOG_INFO("Worker PEs set for workqueue (count=%u)", count);
}

void pci_cfg_workqueue_clear_worker_pes(void)
{
	uint32_t i;

	if (pci_cfg_wq == NULL)
		return;

	pthread_mutex_lock(&pci_cfg_wq->mutex);
	for (i = 0; i < pci_cfg_wq->worker_pe_count; i++)
		pci_cfg_wq->worker_pes[i] = NULL;
	pci_cfg_wq->worker_pe_count = 0;
	pci_cfg_wq->diag_tlp_ctx = NULL;
	pci_cfg_wq->diag_collection_enabled = false;
	pci_cfg_wq->diag_next_controller = 0;
	pthread_mutex_unlock(&pci_cfg_wq->mutex);
	DOCA_LOG_DBG("Worker PEs cleared from workqueue");
}

void pci_cfg_workqueue_set_diag_collection(struct tlp_context *tlp_ctx, bool enabled)
{
	if (pci_cfg_wq == NULL)
		return;

	pthread_mutex_lock(&pci_cfg_wq->mutex);
	pci_cfg_wq->diag_tlp_ctx = enabled ? tlp_ctx : NULL;
	pci_cfg_wq->diag_collection_enabled = enabled;
	pci_cfg_wq->diag_next_controller = 0;
	pthread_mutex_unlock(&pci_cfg_wq->mutex);
	DOCA_LOG_DBG("Worker-side diagnostics %s", enabled ? "enabled" : "disabled");
}

void pci_cfg_workqueue_submit_delayed_destroy(struct tlp_context *tlp_ctx, struct pci_device_config *endpoint)
{
	struct pci_cfg_work_item *work;
	bool expected;

	if (tlp_ctx == NULL || endpoint == NULL)
		return;

	/* Check if unplug is pending (required for destroy to proceed) */
	if (!atomic_load(&endpoint->pending_unplug)) {
		DOCA_LOG_DBG("No pending unplug, skipping delayed destroy");
		return;
	}

	/* Atomically check and set pending_destroy to prevent duplicate submissions.
	 * This is a compare-exchange: only set to true if currently false. */
	expected = false;
	if (!atomic_compare_exchange_strong(&endpoint->pending_destroy, &expected, true)) {
		DOCA_LOG_DBG("Delayed destroy already queued, skipping");
		return;
	}

	work = work_pool_alloc();
	if (work == NULL) {
		DOCA_LOG_ERR("Failed to alloc work item for delayed destroy");
		/* Restore pending_destroy since we failed to enqueue */
		atomic_store(&endpoint->pending_destroy, false);
		return;
	}

	work->type = PCI_CFG_WORK_DELAYED_DESTROY;
	work->dev = NULL; /* Not used for delayed destroy */
	work->data.delayed_destroy.tlp_ctx = tlp_ctx;
	work->data.delayed_destroy.endpoint = endpoint;

	DOCA_LOG_INFO("Delayed device destroy queued");
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_submit_hotplug(struct tlp_context *tlp_ctx, uint32_t dsp_index, bool plug)
{
	struct pci_cfg_work_item *work;

	if (tlp_ctx == NULL)
		return;

	work = work_pool_alloc();
	if (work == NULL) {
		DOCA_LOG_ERR("Failed to alloc work item for hotplug event DSP[%u]", dsp_index);
		return;
	}

	work->type = PCI_CFG_WORK_HOTPLUG_EVENT;
	work->dev = NULL; /* Not associated with a specific vnet_pci_device */
	work->data.hotplug_event.tlp_ctx = tlp_ctx;
	work->data.hotplug_event.dsp_index = dsp_index;
	work->data.hotplug_event.plug = plug;

	DOCA_LOG_INFO("Hotplug event queued: DSP[%u] %s (async - main thread stays on PE progress)",
		      dsp_index,
		      plug ? "plug" : "unplug");
	pci_cfg_workqueue_enqueue(work);
}

void pci_cfg_workqueue_submit_speed_change(struct vnet_pci_dev_controller *controller, uint32_t new_speed)
{
	struct pci_cfg_work_item *work;
	uint32_t reset_generation;

	if (!vnet_controller_get_valid_reset_generation(controller, "Speed change", &reset_generation))
		return;

	work = work_pool_alloc();
	if (work == NULL) {
		DOCA_LOG_ERR("Failed to alloc work item for speed change");
		return;
	}

	work->type = PCI_CFG_WORK_SPEED_CHANGE;
	work->dev = NULL;
	work->data.speed_change.controller = controller;
	work->data.speed_change.new_speed = new_speed;
	work->reset_generation = reset_generation;

	DOCA_LOG_INFO("Speed change to %u Mbps queued (async)", new_speed);
	pci_cfg_workqueue_enqueue(work);
}

/* Thread argument for joinable MQ start thread */
struct mq_start_thread_arg {
	struct pci_cfg_workqueue *wq;
	struct vnet_pci_dev_controller *controller;
	struct mq_start_thread_node *thread_node;
	uint16_t old_qps;
	uint16_t new_qps;
};

/*
 * Joinable thread function for MQ start QPs.
 *
 * Each device's start_additional_queue_pairs runs in its own thread so all
 * devices can start their 126 QPs in parallel (~8s each) instead of serially.
 *
 * IMPORTANT: The CVQ ctrl_req response has already been sent to the host
 * BEFORE this thread is created. This is the key to unblocking the host:
 * the host's virtnet_send_command() was busy-polling waiting for the CVQ
 * response, and now it returns immediately so the host can probe the next
 * device while we start QPs in the background.
 *
 * Safety: each thread operates on its own controller's VQ handles - no
 * cross-controller data access. The DOCA VQ start/set_conf APIs are
 * per-controller and don't share state across controllers.
 */
static void *mq_start_thread_func(void *arg)
{
	struct mq_start_thread_arg *mq_arg = (struct mq_start_thread_arg *)arg;
	struct pci_cfg_workqueue *wq = mq_arg->wq;
	bool sem_acquired = false;

	if (wq == NULL)
		goto out;
	pthread_mutex_lock(&wq->mutex);
	if (pci_cfg_wq != wq || wq->mq_start_teardown || wq->shutdown) {
		pthread_mutex_unlock(&wq->mutex);
		goto out;
	}
	pthread_mutex_unlock(&wq->mutex);

	vnet_pci_device_pin_current_thread(VNET_THREAD_AFFINITY_MQ, "MQ start");

	/* Acquire semaphore slot - blocks if MQ_START_MAX_CONCURRENT threads
	 * are already running start_additional_queue_pairs. This prevents
	 * OOM on DPU when many devices start 127 QPs simultaneously. */
	DOCA_LOG_INFO("MQ start thread waiting for semaphore slot (max %d concurrent)...", MQ_START_MAX_CONCURRENT);
	while (sem_wait(&wq->mq_start_sem) != 0) {
		if (errno == EINTR)
			continue;
		DOCA_LOG_ERR("MQ start thread failed to acquire semaphore: %d", errno);
		goto out;
	}
	sem_acquired = true;

	pthread_mutex_lock(&wq->mutex);
	if (pci_cfg_wq != wq || wq->mq_start_teardown || wq->shutdown) {
		pthread_mutex_unlock(&wq->mutex);
		goto out;
	}
	pthread_mutex_unlock(&wq->mutex);
	DOCA_LOG_INFO("MQ start thread acquired semaphore slot");

	vnet_pci_dev_execute_mq_start_qps(mq_arg->controller, mq_arg->old_qps, mq_arg->new_qps);

out:
	if (sem_acquired)
		sem_post(&wq->mq_start_sem);
	if (wq != NULL) {
		pthread_mutex_lock(&wq->mutex);
		mq_arg->thread_node->completed = true;
		pthread_mutex_unlock(&wq->mutex);
	}
	free(mq_arg);
	atomic_fetch_sub(&mq_start_threads_active, 1);
	return NULL;
}

void pci_cfg_workqueue_submit_mq_start_qps(struct vnet_pci_dev_controller *controller,
					   uint16_t old_qps,
					   uint16_t new_qps)
{
	struct mq_start_thread_arg *arg;
	struct mq_start_thread_node *thread_node;
	struct pci_cfg_workqueue *wq = pci_cfg_wq;
	pthread_t thread;
	int ret;

	if (controller == NULL || wq == NULL)
		return;

	pci_cfg_workqueue_reap_completed_mq_threads(wq);

	pthread_mutex_lock(&wq->mutex);
	if (pci_cfg_wq != wq || wq->mq_start_teardown || wq->shutdown) {
		pthread_mutex_unlock(&wq->mutex);
		return;
	}
	atomic_fetch_add(&mq_start_threads_active, 1);
	pthread_mutex_unlock(&wq->mutex);

	/* Allocate thread argument (freed by thread after completion) */
	arg = malloc(sizeof(struct mq_start_thread_arg));
	if (arg == NULL) {
		DOCA_LOG_ERR("Failed to allocate MQ start thread arg");
		atomic_fetch_sub(&mq_start_threads_active, 1);
		return;
	}

	thread_node = malloc(sizeof(*thread_node));
	if (thread_node == NULL) {
		DOCA_LOG_ERR("Failed to allocate MQ start thread tracker");
		free(arg);
		atomic_fetch_sub(&mq_start_threads_active, 1);
		return;
	}

	arg->wq = wq;
	arg->controller = controller;
	arg->thread_node = thread_node;
	arg->old_qps = old_qps;
	arg->new_qps = new_qps;

	thread_node->completed = false;
	thread_node->next = NULL;

	/* The CVQ response was already sent to the host, so this can run in
	 * the background. Track the joinable thread so shutdown can wait for it. */
	pthread_mutex_lock(&wq->mutex);
	if (pci_cfg_wq != wq || wq->mq_start_teardown || wq->shutdown) {
		wq->dropped_count++;
		pthread_mutex_unlock(&wq->mutex);
		free(thread_node);
		free(arg);
		atomic_fetch_sub(&mq_start_threads_active, 1);
		return;
	}

	ret = pthread_create(&thread, NULL, mq_start_thread_func, arg);

	if (ret != 0) {
		pthread_mutex_unlock(&wq->mutex);
		DOCA_LOG_ERR("Failed to create MQ start thread: %d", ret);
		free(thread_node);
		free(arg);
		atomic_fetch_sub(&mq_start_threads_active, 1);
		return;
	}

	thread_node->thread = thread;
	thread_node->next = wq->mq_threads;
	wq->mq_threads = thread_node;
	wq->mq_thread_count++;
	pthread_mutex_unlock(&wq->mutex);

	DOCA_LOG_INFO("MQ start QPs %u -> %u spawned as joinable thread (parallel execution)", old_qps, new_qps);
}

static bool vnet_pci_device_data_vq_is_deferred(struct vnet_pci_device *dev, uint16_t vq_index)
{
	struct vnet_pci_dev_resources *resources;
	struct vnet_pci_dev_controller *controller;
	uint16_t cvq_index;
	uint16_t qp_idx;

	resources = dev->cb_arg;
	if (resources == NULL || resources->tlp_ctx == NULL)
		return false;

	controller = &resources->tlp_ctx->vnet_controller[dev->pf_index];
	cvq_index = VNET_CVQ_INDEX(controller->max_queue_pairs);
	if (vq_index >= cvq_index)
		return false;

	qp_idx = vq_index / 2;
	return atomic_load(&controller->deferred_mq.initial_data_qps_deferred) ||
	       qp_idx >= atomic_load(&controller->num_active_qps);
}

static bool vnet_pci_device_reset_status_is_pending(struct vnet_pci_device *dev)
{
	struct vnet_pci_dev_resources *resources;
	struct vnet_pci_dev_controller *controller;

	if (dev == NULL || dev->pf_index < 0)
		return false;

	resources = dev->cb_arg;
	if (resources == NULL || resources->tlp_ctx == NULL || (uint32_t)dev->pf_index >= resources->tlp_ctx->num_ep)
		return false;

	controller = &resources->tlp_ctx->vnet_controller[dev->pf_index];
	return atomic_load(&controller->reset_status_state) != VNET_RESET_STATUS_IDLE;
}

static void vnet_pci_device_apply_reset_status_release(struct vnet_pci_device *dev)
{
	struct vnet_pci_dev_resources *resources;
	struct vnet_pci_dev_controller *controller;
	struct vnet_virtio_common_config *pci_cfg;
	uint32_t expected_state = VNET_RESET_STATUS_RELEASE_PENDING;

	if (dev == NULL || dev->pf_index < 0)
		return;

	resources = dev->cb_arg;
	if (resources == NULL || resources->tlp_ctx == NULL || (uint32_t)dev->pf_index >= resources->tlp_ctx->num_ep)
		return;

	controller = &resources->tlp_ctx->vnet_controller[dev->pf_index];
	if (!atomic_compare_exchange_strong(&controller->reset_status_state, &expected_state, VNET_RESET_STATUS_IDLE))
		return;

	pci_cfg = &dev->pci_cfg;
	if (pci_cfg->device_status == VNET_VIRTIO_DEVICE_STATUS_NEEDS_RESET) {
		pci_cfg->device_status = 0;
		dev->prev_status = 0;
		DOCA_LOG_INFO("RESET: TLP thread released host-visible reset hold");
	}
	atomic_store(&dev->cancel_in_progress, false);
}

static void pci_cfg_workqueue_submit_vq_config(struct vnet_pci_device *dev, uint16_t vq_index)
{
	struct pci_cfg_work_item *work;

	if (dev == NULL)
		return;

	/* Drop VQ config submissions during device reset to prevent race.
	 * Reset will clear all VQ state, so any pending config would be stale. */
	if (pci_cfg_wq != NULL && pci_cfg_wq->reset_in_progress) {
		DOCA_LOG_DBG("VQ%d config dropped - device reset in progress", vq_index);
		return;
	}

	if (vnet_pci_device_data_vq_is_deferred(dev, vq_index))
		return;

	work = work_pool_alloc();
	if (work == NULL)
		return;

	work->type = PCI_CFG_WORK_VQ_CONFIG;
	work->dev = dev;
	work->data.vq_config.vq_index = vq_index;
	work->reset_generation = vnet_pci_device_get_reset_generation(dev);

	DOCA_LOG_DBG("VQ%d config queued", vq_index);
	pci_cfg_workqueue_enqueue(work);
}

/************************************************************************
 ******              VirtIO Device Template Creation               ******
 ************************************************************************/

/**
 * @brief Setup basic PCI configuration header
 *
 * Configures the standard PCI Type 0 configuration header with VirtIO-specific
 * values including vendor ID, device class, and capabilities pointer.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_pci_header(struct pcie_virtio_dev *dev)
{
	struct pci_config_header_type0 *regs = &dev->cfg.regs;

	regs->vendor_id = VIRTIO_PCI_VENDOR_ID; /* Red Hat VirtIO */
	regs->subsystem_vendor_id = VIRTIO_PCI_VENDOR_ID;
	regs->device_id = VIRTIO_PCI_DEVICE_ID_BASE; /* Will be customized later */
	regs->status = PCI_STATUS_CAP_LIST;	     /* Capabilities list present */
	regs->class_code = PCI_CLASS_NETWORK;	     /* Default to network */
	regs->subclass = 0x00;			     /* Ethernet controller */
	regs->revision_id = 1;
	regs->capabilities_ptr = offsetof(struct pcie_virtio_dev, cfg.pcie_cap);

	/* Configure BAR0 as 64-bit prefetchable memory */
	regs->base_address[VNET_VIRTIO_BAR_ID] = PCI_BASE_ADDRESS_MEM_PREFETCH | PCI_BASE_ADDRESS_MEM_TYPE_64;
}

/**
 * @brief Setup PCIe Express capability
 *
 * Configures the PCIe capability structure with appropriate device type,
 * version, and link characteristics for VirtIO devices.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_pcie_capability(struct pcie_virtio_dev *dev)
{
	struct pcie_capability *pcie_cap = &dev->cfg.pcie_cap;

	/* PCIe capability header */
	pcie_cap->cap.cap_id = PCI_CAP_ID_EXP;
	pcie_cap->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.msix_cap);

	/* PCIe capability registers */
	pcie_cap->pcie_cap = 0x0002;	/* PCIe Version 2, Endpoint Type 0 */
	pcie_cap->dev_cap = 0x00008001; /* Max Payload Size = 256 bytes */
	/*
	 * PCIe Link Capability encoding:
	 *   Bits [3:0]  = Max Link Speed: 5 = Gen5 (32 GT/s)
	 *   Bits [9:4]  = Max Link Width: 8 = x8
	 *   Bits [11:10]= ASPM Support: 3 = L0s and L1
	 *   Combined lower bits: 0x85 (speed=5, width=8<<4=0x80)
	 */
	pcie_cap->link_cap = 0x00011C85; /* Gen5 32GT/s, x8 width, L0s/L1 supported */
	pcie_cap->link_status = 0x1085;	 /* Gen5, x8 width, link training complete */
}

/**
 * @brief Setup MSI-X capability
 *
 * Configures MSI-X capability with table and PBA locations within the BAR.
 * Table size is set to support maximum number of vectors.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_msix_capability(struct pcie_virtio_dev *dev)
{
	struct msix_capability *msix_cap = &dev->cfg.msix_cap;

	/* MSI-X capability header */
	msix_cap->cap.cap_id = PCI_CAP_ID_MSIX;
	msix_cap->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.common_cfg);

	/* MSI-X control and table configuration */
	msix_cap->msgctl = (VNET_PCI_DEV_NUM_MSIX - 1); /* MSI-X Disabled by default | Table Size (63) */
	msix_cap->table_offset = VNET_VIRTIO_MSIX_TABLE_OFFSET | VNET_VIRTIO_BAR_ID;
	msix_cap->pba_offset = VNET_VIRTIO_MSIX_PBA_OFFSET | VNET_VIRTIO_BAR_ID;
}

/**
 * @brief Setup VirtIO Common Configuration capability
 *
 * Configures the VirtIO common configuration capability that provides
 * access to device-wide configuration registers.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_virtio_common_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *common_cfg = &dev->cfg.common_cfg;

	/* Capability header */
	common_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	common_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.notify_cfg);

	/* VirtIO capability fields */
	common_cfg->cap_len = sizeof(struct virtio_pci_capability);
	common_cfg->cfg_type = VIRTIO_PCI_CAP_COMMON_CFG;
	common_cfg->bar = 0;
	common_cfg->offset = 0; /* Uses offset 0 for common config */
	common_cfg->length = VNET_VIRTIO_PCI_CFG_LEN;
}

/**
 * @brief Setup VirtIO Notification capability
 *
 * Configures the notification capability that provides doorbell registers
 * for queue notifications with proper stride configuration.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_virtio_notify_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_notify_capability *notify_cfg = &dev->cfg.notify_cfg;

	/* Base capability header */
	notify_cfg->base.cap.cap_id = PCI_CAP_ID_VNDR;
	notify_cfg->base.cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.isr_cfg);

	/* VirtIO notification capability fields */
	notify_cfg->base.cap_len = sizeof(struct virtio_pci_notify_capability);
	notify_cfg->base.cfg_type = VIRTIO_PCI_CAP_NOTIFY_CFG;
	notify_cfg->base.bar = 0;
	notify_cfg->base.offset = VNET_VIRTIO_DB_OFFSET; /* Doorbell offset */
	notify_cfg->base.length = VNET_VIRTIO_DB_LEN;

	/* Notification multiplier for doorbell stride */
	notify_cfg->notify_off_multiplier = (1 << VNET_VIRTIO_DB_STRIDE);
}

/**
 * @brief Setup VirtIO ISR Status capability
 *
 * Configures the ISR status capability that provides interrupt status
 * information for the device.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_virtio_isr_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *isr_cfg = &dev->cfg.isr_cfg;

	/* Capability header */
	isr_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	isr_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.device_cfg);

	/* VirtIO ISR capability fields */
	isr_cfg->cap_len = sizeof(struct virtio_pci_capability);
	isr_cfg->cfg_type = VIRTIO_PCI_CAP_ISR_CFG;
	isr_cfg->bar = 0;
	isr_cfg->offset = VNET_VIRTIO_ISR_CFG_OFFSET;
	isr_cfg->length = VNET_VIRTIO_ISR_CFG_LEN;
}

/**
 * @brief Setup VirtIO Device Configuration capability
 *
 * Configures the device-specific configuration capability that provides
 * access to device type specific configuration registers.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_virtio_device_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_capability *device_cfg = &dev->cfg.device_cfg;

	/* Capability header */
	device_cfg->cap.cap_id = PCI_CAP_ID_VNDR;
	device_cfg->cap.next_ptr = offsetof(struct pcie_virtio_dev, cfg.pci_cfg);

	/* VirtIO device capability fields */
	device_cfg->cap_len = sizeof(struct virtio_pci_capability);
	device_cfg->cfg_type = VIRTIO_PCI_CAP_DEVICE_CFG;
	device_cfg->bar = 0;
	device_cfg->offset = VNET_VIRTIO_DEV_CFG_OFFSET;
	device_cfg->length = VNET_VIRTIO_DEV_CFG_LEN;
}

/**
 * @brief Setup VirtIO PCI Configuration Access capability
 *
 * Configures the PCI configuration access capability (end of capability chain).
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_virtio_pci_cfg_capability(struct pcie_virtio_dev *dev)
{
	struct virtio_pci_cfg_capability *pci_cfg = &dev->cfg.pci_cfg;

	/* Base capability header (end of chain) */
	pci_cfg->base.cap.cap_id = PCI_CAP_ID_VNDR;
	pci_cfg->base.cap.next_ptr = 0; /* End of capability chain */

	/* VirtIO PCI cfg capability fields */
	pci_cfg->base.cap_len = sizeof(struct virtio_pci_cfg_capability);
	pci_cfg->base.cfg_type = VIRTIO_PCI_CAP_PCI_CFG;
	pci_cfg->base.bar = 0;
	pci_cfg->base.offset = 0;
	pci_cfg->base.length = 0;
}

/**
 * @brief Setup BAR memory mapping configuration
 *
 * Configures the BAR size and mapping properties for the VirtIO device.
 *
 * @param[out] dev VirtIO device template to configure
 */
static void vnet_pci_device_setup_bar_mapping(struct pcie_virtio_dev *dev)
{
	dev->bar64_map[VNET_VIRTIO_BAR_ID].log_size = VNET_VIRTIO_BAR_LOG_SIZE;
}

/**
 * @brief Create VirtIO network device template
 *
 * Creates a complete VirtIO device template with all required capabilities
 * configured according to VirtIO specification.
 *
 * @return Complete VirtIO device template ready for use
 */
static struct pcie_virtio_dev vnet_pci_device_create_virtio_template(void)
{
	struct pcie_virtio_dev template = {0}; /* Zero-initialize */

	/* Setup each component in logical order */
	vnet_pci_device_setup_pci_header(&template);
	vnet_pci_device_setup_pcie_capability(&template);
	vnet_pci_device_setup_msix_capability(&template);
	vnet_pci_device_setup_virtio_common_capability(&template);
	vnet_pci_device_setup_virtio_notify_capability(&template);
	vnet_pci_device_setup_virtio_isr_capability(&template);
	vnet_pci_device_setup_virtio_device_capability(&template);
	vnet_pci_device_setup_virtio_pci_cfg_capability(&template);
	vnet_pci_device_setup_bar_mapping(&template);

	return template;
}

/************************************************************************
 ******             Helper Functions for TLP Processing            ******
 ************************************************************************/

/**
 * Get TLP request type from TLP request header buffer
 *
 * @tlp_req [in]: Pointer to TLP request
 *
 * @return: TLP request type local enumeration value based on PCI specification
 */
static inline enum tlp_req_type get_tlp_req_type(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint8_t type = GET_TLP_REQ_TYPE(tlp_req_header);

	switch (type) {
	case TLP_REQ_TYPE_MEMORY_READ_WRITE:
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_MEMORY_WRITE;
		} else {
			return TLP_REQ_TYPE_MEMORY_READ;
		}
	case TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0:
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0;
		} else {
			return TLP_REQ_TYPE_CONFIG_READ_TYPE_0;
		}
	case TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1:
		if (fmt & TLP_FMT_3DW_W_DATA) {
			return TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1;
		} else {
			return TLP_REQ_TYPE_CONFIG_READ_TYPE_1;
		}
	default:
		return TLP_REQ_TYPE_INVALID;
	}
}

/**
 * @brief Map first/last DW byte enable to mask based on PCI specification
 *
 * @param[in] dw_be First/Last DW byte enable value
 *
 * @return: Corresponding byte mask based on PCI specification
 */
static inline uint32_t map_dw_be_to_mask(const uint32_t dw_be)
{
	static const uint32_t dw_be_to_mask[] = {
		0x00000000, /* 0000 */
		0x000000FF, /* 0001 */
		0x0000FF00, /* 0010 */
		0x0000FFFF, /* 0011 */
		0x00FF0000, /* 0100 */
		0x00FF00FF, /* 0101 */
		0x00FFFF00, /* 0110 */
		0x00FFFFFF, /* 0111 */
		0xFF000000, /* 1000 */
		0xFF0000FF, /* 1001 */
		0xFF00FF00, /* 1010 */
		0xFF00FFFF, /* 1011 */
		0xFFFF0000, /* 1100 */
		0xFFFF00FF, /* 1101 */
		0xFFFFFF00, /* 1110 */
		0xFFFFFFFF, /* 1111 */
	};

	return dw_be_to_mask[dw_be];
}

/**
 * @brief Prepare TLP completion header DW0 (Double Word 0)
 *
 * Sets up the first double word of a TLP completion header with format,
 * type, tag bits, and length fields according to PCIe specification.
 *
 * @param[in] tlp_req Original TLP request to respond to
 * @param[in] cmpl_fmt Completion format (3DW or 4DW, with/without data)
 * @param[in] cmpl_length Completion length in double words
 *
 * @note Preserves request TAG fields for proper transaction matching
 */
static void vnet_pci_dev_tlp_cpl_prep_dw0(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					  const uint8_t cmpl_fmt,
					  const uint8_t cmpl_length)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	header_dw[0] = 0; /* clear DW0 */

	/* Set format and type for completion - CRITICAL: include TAG bits */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), cmpl_fmt, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), TLP_TYPE_COMPLETION, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(23, 23), GET_TLP_REQ_TAG9(tlp_req_header), &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(19, 19), GET_TLP_REQ_TAG8(tlp_req_header), &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), cmpl_length, &header_dw[0]);
}

/**
 * @brief Prepare TLP completion header DW1 and DW2
 *
 * Sets up double words 1 and 2 of TLP completion header with completer ID,
 * completion status, byte count, and requester information.
 *
 * @param[in] tlp_req Original TLP request to respond to
 * @param[in] completer_id PCI device ID of the completing device
 * @param[in] cmpl_status Completion status (SC, UR, CRS, CA)
 * @param[in] byte_count Number of bytes in completion data
 *
 * @note Copies TAG and requester ID from original request for matching
 */
static void vnet_pci_dev_tlp_cpl_prep_dw1_2(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					    const uint32_t completer_id,
					    const uint8_t cmpl_status,
					    const uint8_t byte_count)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	/* DW1: completer_id[31:16], cmpl_status[15:13], byte_cnt[11:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), completer_id, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 13), cmpl_status, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(11, 0), byte_count, &header_dw[1]);

	/* DW2: requester_id[31:16], tag[15:8], lower_addr[6:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), GET_TLP_REQ_REQ_ID(tlp_req_header), &header_dw[2]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), GET_TLP_REQ_TAG(tlp_req_header), &header_dw[2]);
}

/* Forward declaration - full definition is later in file */
static inline void set_tlp_req_completion_header(void *tlp_response_header,
						 struct doca_devemu_pci_tlp_channel_req *tlp_req,
						 uint8_t cmpl_fmt,
						 uint8_t cmpl_status,
						 uint8_t cmpl_length,
						 uint16_t byte_count,
						 uint16_t completer_id_override);

/**
 * @brief Prepare TLP configuration completion response
 *
 * Sets up the TLP completion header with proper format, status, and data length.
 * Used for responding to configuration read/write requests.
 *
 * @param[in,out] tlp_req TLP request to prepare completion for
 * @param[in] cmpl_fmt Completion format (3DW vs 4DW)
 * @param[in] cmpl_status Completion status (success, error, etc.)
 * @param[in] cmpl_length Length of completion data in DWords
 * @param[in] byte_count Actual byte count for partial completions
 */
static void vnet_pci_dev_tlp_cfg_cpl_prep(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					  enum tlp_format cmpl_fmt,
					  enum tlp_completion_status cmpl_status,
					  const uint8_t cmpl_length,
					  const uint8_t byte_count)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	memset(header_dw, 0, 3 * sizeof(uint32_t)); /* clear completion header DWs */

	vnet_pci_dev_tlp_cpl_prep_dw0(tlp_req, cmpl_fmt, cmpl_length);

	/* cfg requests are always bdf routed */
	uint16_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint16_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint16_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);
	uint16_t completer_id = (bus << 8) | (device << 3) | function;

	vnet_pci_dev_tlp_cpl_prep_dw1_2(tlp_req, completer_id, cmpl_status, byte_count);
}

/**
 * @brief Prepare TLP completion for MMIO operations
 *
 * Creates completion headers for memory-mapped I/O read responses.
 * Uses the device's BDF (Bus, Device, Function) as completer ID.
 *
 * @param[in] virtio_dev VirtIO device context for completer ID
 * @param[in] tlp_req Original MMIO TLP request to respond to
 * @param[in] cmpl_fmt Completion format (3DW or 4DW, with/without data)
 * @param[in] cmpl_status Completion status (SC, UR, CRS, CA)
 * @param[in] cmpl_length Completion length in double words
 * @param[in] byte_count Number of bytes in completion data
 *
 * @note Uses device BDF for proper PCIe routing
 */
static void vnet_pci_dev_tlp_mmio_cpl_prep(struct vnet_pci_device *virtio_dev,
					   struct doca_devemu_pci_tlp_channel_req *tlp_req,
					   enum tlp_format cmpl_fmt,
					   enum tlp_completion_status cmpl_status,
					   const uint8_t cmpl_length,
					   const uint8_t byte_count)
{
	uint32_t *header_dw = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	memset(header_dw, 0, 3 * sizeof(uint32_t)); /* clear completion header DWs */
	vnet_pci_dev_tlp_cpl_prep_dw0(tlp_req, cmpl_fmt, cmpl_length);

	const uint16_t completer_id = (virtio_dev->bus << 8) | (virtio_dev->device << 3) | virtio_dev->function;

	vnet_pci_dev_tlp_cpl_prep_dw1_2(tlp_req, completer_id, cmpl_status, byte_count);
}

/* VirtIO BAR address calculation helper */
static inline uint64_t vnet_pci_dev_bar_offset(struct vnet_pci_device *dev, const uint64_t addr)
{
	/* Calculate offset from BAR0 base address
	 * For our simplified structure: BAR0 is at dw_regs[4] (low) + dw_regs[5] (high) for 64-bit BAR
	 */
	uint64_t bar0_low = dev->pcie_dev.dw_regs[4] & ~0xF; /* Clear flag bits */
	uint64_t bar0_high = dev->pcie_dev.dw_regs[5];
	uint64_t bar0_address = bar0_low | (bar0_high << 32);

	return addr - bar0_address;
}

/**
 * @brief Build new value for configuration space write
 *
 * Builds the new value for a configuration space write operation based on the
 * register number, input data, byte enable mask, and current value.
 *
 * @param[in] ext_reg_num Register number
 * @param[in] dw_input Input data to write
 * @param[in] be_mask Byte enable mask for partial writes
 * @param[in] cur_value Current value of the register
 * @return Updated register value
 */
static uint32_t vnet_pci_dev_build_cfg_write_new_value(struct vnet_pci_device *virtio_dev,
						       const uint32_t ext_reg_num,
						       const uint32_t dw_input,
						       const uint32_t be_mask,
						       const uint32_t cur_value)
{
	uint32_t new_value = cur_value;
	uint32_t final_mask = 0;

	switch (ext_reg_num) {
	case 1:				    /* Command/Status - Mixed RW/W1C */
		if (be_mask & 0x0000FFFF) { /* Command Register (lower 16 bits) - for Standard RW */
			final_mask = be_mask & 0x0000FFFF & VIRTIO_PCI_CAP_CMD_REG_RW_BITS;
			new_value = (new_value & ~final_mask) | (dw_input & be_mask & final_mask);
		}

		if (be_mask & 0xFFFF0000) { /* Status Register (upper 16 bits) - for Write-1-to-Clear */
			final_mask = be_mask & 0xFFFF0000 & (VIRTIO_PCI_CAP_STATUS_REG_W1C_BITS << 16);
			new_value = new_value & ~(dw_input & be_mask & final_mask); /* W1C: clear where input has 1s */
		}
		break;
	case 4: /* BAR0 Low - based on PCIe spec */
	{
		const int log_size = virtio_dev->pcie_dev.bar64_map[VNET_VIRTIO_BAR_ID].log_size;
		uint64_t bar_size = ((uint64_t)0x1 << log_size);
		if ((dw_input & be_mask) == 0xFFFFFFFF) {
			new_value = (~(bar_size - 1) & 0xFFFFFFFF) | (cur_value & 0x0000000F);
		} else {
			new_value = (dw_input & be_mask) | (cur_value & 0x0000000F);
		}
		break;
	}
	case 5: /* BAR0 High - based on PCIe spec */
	{
		const int log_size = virtio_dev->pcie_dev.bar64_map[VNET_VIRTIO_BAR_ID].log_size;
		uint64_t bar_size = ((uint64_t)0x1 << log_size);
		if ((dw_input & be_mask) == 0xFFFFFFFF) {
			new_value = ((~(bar_size - 1) >> 32) & 0xFFFFFFFF);
		} else {
			new_value = (dw_input & be_mask);
		}
		break;
	}
	case VIRTIO_NET_MSI_X_REGISTER_NUM:	   /* MSIX capability - Partial RW */
		final_mask = be_mask & 0xC0000000; /* MSI-X Enable (bit 31) and Function Mask (bit 30) are RW */
		new_value = (cur_value & ~final_mask) | (dw_input & final_mask);
		break;
	default: /* other registers are read-only */
		break;
	}

	return new_value;
}

/**
 * @brief Find VirtIO device by TLP request addressing for configuration space
 *
 * Extracts Bus/Device/Function from TLP header and looks up the corresponding
 * VirtIO device from the enumerated devices list.
 *
 * @param[in] tlp_req TLP request containing B/D/F addressing
 * @return Pointer to VirtIO device if found, NULL otherwise
 */
static struct vnet_pci_device *vnet_pci_device_find_by_tlp_cfg(struct doca_devemu_pci_tlp_channel_req *tlp_req)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint8_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint8_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint8_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);

	return vnet_pci_dev_space[bus][device][function];
}

/**
 * @brief Find VirtIO device by BAR memory address
 *
 * Searches enumerated devices to find which device owns the given memory address.
 * Used for MMIO operations to route to the correct VirtIO device.
 *
 * @param[in] addr Memory address from MMIO TLP
 * @return Pointer to owning VirtIO device, NULL if no match found
 */
static struct vnet_pci_device *vnet_pci_device_find_by_bar_addr(const uint64_t addr)
{
	struct vnet_pci_device *dev;

	LIST_FOREACH(dev, &vnet_pci_dev_enumerated_devs, entry)
	{
		/* in generally we have to match all bars, but for the virtio we only need one */
		const int log_size = dev->pcie_dev.bar64_map[VNET_VIRTIO_BAR_ID].log_size;
		/* clear type bits - access BAR0 via raw dw_regs since we don't have cfg union */
		uint64_t bar0_low = dev->pcie_dev.dw_regs[4] & ~0xF; /* BAR0 low 32 bits */
		uint64_t bar0_high = dev->pcie_dev.dw_regs[5];	     /* BAR0 high 32 bits */
		const uint64_t bar_base_addr = bar0_low | (bar0_high << 32);

		if (addr >= bar_base_addr && addr < bar_base_addr + (1 << log_size))
			return dev;
	}
	return NULL;
}

/*
 * Get PF index for a given endpoint device
 *
 * @tlp_ctx [in]: TLP context
 * @dev_cfg [in]: Device configuration
 * @pf_index [out]: Pointer to store the PF index
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t get_pf_index_for_device(struct tlp_context *tlp_ctx, struct pci_device_config *dev_cfg, uint32_t *pf_index)
{
	uint32_t i;

	/* Only endpoint devices have PF indices */
	if (!dev_cfg->is_endpoint)
		return DOCA_ERROR_NOT_SUPPORTED;

	/* Find device index in the devices array */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		if (dev_cfg == &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i]) {
			*pf_index = i;
			return DOCA_SUCCESS;
		}
	}

	/* Device not found in PF range */
	return DOCA_ERROR_NOT_FOUND;
}

/**
 * @brief Enumerate a new VirtIO device during PCIe discovery
 *
 * Creates and registers a new VirtIO device in response to the first PCIe
 * configuration access. This simulates device enumeration during host boot.
 *
 * @param[in] tlp_req TLP request that triggered device discovery
 * @return Pointer to newly enumerated device, NULL on failure
 */
static struct vnet_pci_device *vnet_pci_device_enumerate(struct tlp_context *tlp_ctx,
							 struct doca_devemu_pci_tlp_channel_req *tlp_req,
							 struct pci_device_config *dev_cfg)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint8_t bus = GET_TLP_REQ_BUS(tlp_req_header);
	uint8_t device = GET_TLP_REQ_DEVICE(tlp_req_header);
	uint8_t function = GET_TLP_REQ_FUNCTION(tlp_req_header);

	// Search from the enumerated list. We will recapture the new Bus number in case Bus number is changed.
	struct vnet_pci_device *enumerated_dev = NULL;
	for (struct vnet_pci_device *dev = LIST_FIRST(&vnet_pci_dev_enumerated_devs); dev != NULL;
	     dev = LIST_NEXT(dev, entry)) {
		if (dev->bus == bus && dev->device == device && dev->function == function) {
			enumerated_dev = dev;
			break;
		}
	}

	if (enumerated_dev)
		return enumerated_dev;

	uint32_t pf_index;

	/* Internal trace for dev_cfg correlation, not user-facing */
	DOCA_LOG_DBG("enumerate trace: probe bus=%02x dev_cfg=%p EP[0]=%p EP[1]=%p",
		     bus,
		     (void *)dev_cfg,
		     (void *)&tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + 0],
		     (tlp_ctx->num_ep > 1) ? (void *)&tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + 1] : NULL);

	doca_error_t ret = get_pf_index_for_device(tlp_ctx, dev_cfg, &pf_index);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get PF index for device %02x:%02x.%x", bus, device, function);
		return NULL;
	}

	struct vnet_pci_device *virtio_dev = &tlp_ctx->virtio_dev[pf_index];

	/* Already enumerated: handle bus reassignment (host PCI rescan or pci=realloc compaction) */
	if (virtio_dev->is_enumerated) {
		uint8_t old_bus = virtio_dev->bus;
		if (old_bus != bus) {
			DOCA_LOG_INFO("VNet pf_index=%u: BDF reassigned %02x:%02x.%x -> %02x:%02x.%x",
				      pf_index,
				      old_bus,
				      device,
				      function,
				      bus,
				      device,
				      function);
			vnet_pci_dev_space[old_bus][device][function] = NULL;
			virtio_dev->bus = bus;
			vnet_pci_dev_space[bus][device][function] = virtio_dev;
		}
		return virtio_dev;
	}

	/* First-time enumeration. The bus value is captured during the host's PCI
	 * probe pass and may be reassigned by a subsequent pci=realloc compaction
	 * pass (common on Dell systems with wide BIOS bus reservations). The
	 * authoritative final BDF is shown when the device reaches DRIVER_OK
	 * ("VNet: now OPERATIONAL" log). Demoted to DBG so it never misleads
	 * users comparing against host lspci output. */
	virtio_dev->is_enumerated = true;
	virtio_dev->bus = bus;
	vnet_pci_dev_space[bus][device][function] = virtio_dev;
	LIST_INSERT_HEAD(&vnet_pci_dev_enumerated_devs, virtio_dev, entry);

	DOCA_LOG_DBG("VNet pf_index=%u: first probed at %02x:%02x.%x (transient)", pf_index, bus, device, function);
	return virtio_dev;
}

/************************************************************************
 ******                    TLP Request Handlers                    ******
 ************************************************************************/

/**
 * @brief Handle PCIe Type 0 Configuration Write TLP
 *
 * Processes configuration space write operations from the host during device
 * initialization and runtime configuration. Handles writes to PCI config space,
 * VirtIO capabilities, and device-specific registers. Updates device state and
 * triggers appropriate responses.
 *
 * @param[in] tlp_req Configuration write TLP request from host
 *
 * @note Automatically handles device enumeration if device not found
 * @note Validates write operations and generates appropriate completions
 * @note Critical for VirtIO device status transitions and feature negotiation
 */
static void vnet_pci_dev_handle_cfg_write0(struct doca_devemu_pci_tlp_channel_req *tlp_req, struct tlp_context *tlp_ctx)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t *tlp_req_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req); /* data */
	const uint32_t ext_reg_num = GET_TLP_REQ_EXT_REG_NUM(tlp_req_header);
	uint32_t cur_value, new_value = 0;
	uint32_t be_mask;

	struct vnet_pci_device *virtio_dev = vnet_pci_device_find_by_tlp_cfg(tlp_req);
	if (!virtio_dev) {
		DOCA_LOG_ERR("Failed to find virtual device for TLP request");
		goto fatal_error;
	}

	struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + virtio_dev->pf_index];

	/* Acquire read lock to protect against concurrent hotplug removal.
	 * This ensures tlp_dev remains valid while we use it. */
	pthread_rwlock_rdlock(&ep->endpoint_lock);

	/* Guard against destroyed device - tlp_dev is NULL after hot-unplug destroy.
	 * vnet_pci_dev_space may still have stale entry, so virtio_dev lookup succeeds
	 * but the underlying TLP device is gone. Return UR to host. */
	if (ep->tlp_dev == NULL) {
		pthread_rwlock_unlock(&ep->endpoint_lock);
		DOCA_LOG_DBG("Config write to destroyed device (pf_index=%d) - returning UR", virtio_dev->pf_index);
		goto fatal_error;
	}

	be_mask = map_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	cur_value = VIRTIO_DEV_CONFIG_READ(virtio_dev, ext_reg_num);
	new_value =
		vnet_pci_dev_build_cfg_write_new_value(virtio_dev, ext_reg_num, tlp_req_data[0], be_mask, cur_value);

	if (new_value != cur_value)
		VIRTIO_DEV_CONFIG_WRITE(virtio_dev, ext_reg_num, new_value);

	vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_SC, 0, 4);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, ep->tlp_dev);
	pthread_rwlock_unlock(&ep->endpoint_lock);
	return;

fatal_error:
	vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_UR, 0, 4);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
}

/**
 * @brief Handle PCIe Type 0 Configuration Read TLP
 *
 * Processes configuration space read operations for VirtIO devices during host
 * enumeration and runtime. Returns appropriate configuration data including
 * PCI headers, VirtIO capabilities, and device-specific registers. Handles
 * device enumeration if device not found.
 *
 * @param[in] tlp_req Configuration read TLP request from host
 *
 * @note Automatically enumerates new devices on first access
 * @note Generates appropriate TLP completion responses
 */
static void vnet_pci_dev_handle_cfg_read0(struct doca_devemu_pci_tlp_channel_req *tlp_req, struct tlp_context *tlp_ctx)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	const uint32_t ext_reg_num = GET_TLP_REQ_EXT_REG_NUM(tlp_req_header);
	void *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	uint32_t dw_mask, dw_read, completion_data;

	struct vnet_pci_device *virtio_dev = vnet_pci_device_find_by_tlp_cfg(tlp_req);
	if (!virtio_dev) {
		DOCA_LOG_ERR("Failed to find virtual device for TLP request");
		goto fatal_error;
	}

	struct pci_device_config *ep = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + virtio_dev->pf_index];

	/* Acquire read lock to protect against concurrent hotplug removal.
	 * This ensures tlp_dev remains valid while we use it. */
	pthread_rwlock_rdlock(&ep->endpoint_lock);

	/* Guard against destroyed device - tlp_dev is NULL after hot-unplug destroy.
	 * vnet_pci_dev_space may still have stale entry, so virtio_dev lookup succeeds
	 * but the underlying TLP device is gone. Return UR to host. */
	if (ep->tlp_dev == NULL) {
		pthread_rwlock_unlock(&ep->endpoint_lock);
		DOCA_LOG_DBG("Config read from destroyed device (pf_index=%d) - returning UR", virtio_dev->pf_index);
		goto fatal_error;
	}

	dw_mask = map_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	dw_read = VIRTIO_DEV_CONFIG_READ(virtio_dev, ext_reg_num);
	completion_data = dw_read & dw_mask;

	vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_W_DATA, TLP_CPL_STATUS_SC, 1, 4);

	memcpy(tlp_cpl_data, &completion_data, sizeof(completion_data));
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, ep->tlp_dev);
	pthread_rwlock_unlock(&ep->endpoint_lock);
	return;

fatal_error:
	vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_UR, 0, 4);

	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
}

/**
 * @brief Handle Memory-Mapped I/O Read TLP
 *
 * Processes MMIO read operations to VirtIO device registers.
 * Routes to appropriate handler based on BAR offset (common config, device config, etc.).
 *
 * @param[in] tlp_req MMIO read TLP request
 */
static void vnet_pci_dev_handle_mmio_read(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					  struct tlp_context *tlp_ctx,
					  struct pci_device_config *dev_cfg)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header */
	uint32_t *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	const uint8_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	const uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint32_t *dw_header = (uint32_t *)tlp_req_header;
	uint8_t byte_count = length * sizeof(uint32_t);
	struct vnet_pci_device *virtio_dev;
	uint32_t dw_mask;
	uint64_t addr;
	(void)tlp_ctx;

	if (fmt == TLP_FMT_4DW_NODATA)
		addr = ((uint64_t)be32toh(dw_header[2]) << 32) | (be32toh(dw_header[3]) & ~0x3ULL);
	else
		addr = be32toh(dw_header[2]) & ~0x3;

	/* requests are routed by the mmio address */
	virtio_dev = vnet_pci_device_find_by_bar_addr(addr);
	if (!virtio_dev) {
		DOCA_LOG_DBG("invalid mmio address 0x%lx for endpoint bus=0x%02x", addr, dev_cfg->bus);
		/* CRITICAL: For memory TLPs, DW2 contains address, not BDF.
		 * Must use dev_cfg->bdf as completer_id, not extract from header (which would be garbage).
		 * Using cfg_cpl_prep here was WRONG - it extracts BDF from DW2 causing malformed completion. */
		set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
					      tlp_req,
					      TLP_FMT_CPL_NODATA,
					      TLP_CPL_STATUS_UR,
					      0,
					      4,
					      dev_cfg->bdf);
		doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
		return;
	}

	/* at the moment we only support 1 & 2 dwords reads */
	if (length > 2) {
		DOCA_LOG_ERR("unsupported read length %d", length);
		goto fatal_error;
	}

	if (length == 0) {
		DOCA_LOG_ERR("zero read %d", length);
		goto zero_read;
	}

	dw_mask = map_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	byte_count -= (sizeof(uint32_t) - __builtin_popcount(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header)));
	tlp_cpl_data[0] = (vnet_pci_device_mmio_read32(virtio_dev, addr) & dw_mask);

	if (length == 2) {
		dw_mask = map_dw_be_to_mask(GET_TLP_REQ_LAST_DW_BE(tlp_req_header));
		byte_count -= (sizeof(uint32_t) - __builtin_popcount(GET_TLP_REQ_LAST_DW_BE(tlp_req_header)));
		tlp_cpl_data[1] = (vnet_pci_device_mmio_read32(virtio_dev, addr + 4) & dw_mask);
	}

	vnet_pci_dev_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_W_DATA, TLP_CPL_STATUS_SC, length, byte_count);

	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dev_cfg->tlp_dev);
	return;

fatal_error:
	vnet_pci_dev_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_UR, 0, 4);

	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dev_cfg->tlp_dev);
	return;

zero_read:
	vnet_pci_dev_tlp_mmio_cpl_prep(virtio_dev, tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_SC, 0, 4);

	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, dev_cfg->tlp_dev);
	return;
}

/**
 * @brief Handle Memory-Mapped I/O Write TLP
 *
 * Processes MMIO write operations to VirtIO device registers.
 * Routes to appropriate handler based on BAR offset and triggers device state changes.
 *
 * @param[in] tlp_req MMIO write TLP request
 */
static void vnet_pci_dev_handle_mmio_write(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					   struct tlp_context *tlp_ctx,
					   struct pci_device_config *dev_cfg)
{
	const void *tlp_req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req); /* header + data */
	const uint8_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	const uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint32_t *dw_header = (uint32_t *)tlp_req_header;
	struct vnet_pci_device *virtio_dev;
	uint8_t data_offset;
	uint32_t dw_mask;
	uint64_t addr;
	uint32_t pf_index;

	if (fmt == TLP_FMT_4DW_W_DATA) {
		addr = ((uint64_t)be32toh(dw_header[2]) << 32) | (be32toh(dw_header[3]) & ~0x3ULL);
		data_offset = 4;
	} else {
		addr = be32toh(dw_header[2]) & ~0x3;
		data_offset = 3;
	}

	doca_error_t pf_ret = get_pf_index_for_device(tlp_ctx, dev_cfg, &pf_index);
	if (pf_ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("fatal invalid pf index");
		goto done;
	}
	/* requests are routed by the mmio address */
	virtio_dev = &tlp_ctx->virtio_dev[pf_index];
	if ((uint32_t)virtio_dev->pf_index != pf_index) {
		DOCA_LOG_ERR("fatal invalid mmio address 0x%lx for device %02x:%02x.%x",
			     addr,
			     dev_cfg->bus,
			     dev_cfg->device,
			     dev_cfg->function);
		goto done;
	}

	/* at the moment we only support 1 & 2 dwords writes */
	if (length > 2 || length == 0) {
		DOCA_LOG_ERR("unsupported write length %d", length);
		goto done;
	}

	dw_mask = map_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	vnet_pci_device_mmio_write32(virtio_dev, addr, dw_header[data_offset], dw_mask);

	if (length == 2) {
		dw_mask = map_dw_be_to_mask(GET_TLP_REQ_LAST_DW_BE(tlp_req_header));
		vnet_pci_device_mmio_write32(virtio_dev, addr + 4, dw_header[data_offset + 1], dw_mask);
	}

done:
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 0, dev_cfg->tlp_dev);
	return;
}

/*
 * Find device by BAR address for Memory TLP requests
 *
 * This function acquires the endpoint's read lock when returning an endpoint device.
 * The caller MUST release the lock after completing the TLP request by calling
 * pthread_rwlock_unlock(&dev_cfg->endpoint_lock) when *ep_lock_held is true.
 *
 * @tlp_ctx [in]: TLP context
 * @address [in]: Memory address from TLP
 * @ep_lock_held [out]: Set to true if endpoint read lock is held (caller must unlock)
 * @return: Pointer to device or dummy device if not found
 */
static struct pci_device_config *find_device_by_address(struct tlp_context *tlp_ctx,
							uint64_t address,
							bool *ep_lock_held)
{
	uint32_t i;
	struct vnet_pci_device *virtio_dev;
	struct pci_device_config *dev_cfg;

	*ep_lock_held = false;

	/* Search through all endpoint devices */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		dev_cfg = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
		if (!dev_cfg->is_endpoint)
			continue;

		/* Acquire read lock before checking device state.
		 * This prevents the destroy path from freeing tlp_dev while we use it.
		 * The destroy path takes write lock, so it will wait for us to finish. */
		pthread_rwlock_rdlock(&dev_cfg->endpoint_lock);

		/* Check device validity while holding the lock */
		if (!atomic_load(&dev_cfg->device_present) || dev_cfg->tlp_dev == NULL) {
			pthread_rwlock_unlock(&dev_cfg->endpoint_lock);
			continue;
		}

		/* Check if address falls within BAR0 range (64-bit BAR, 32KB to cover doorbells) */
		uint64_t bar0_base = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
				     (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
		uint64_t bar0_size = BAR_SIZE_32K;

		if (bar0_base != 0 && address >= bar0_base && address < (bar0_base + bar0_size)) {
			/* Found matching device - keep lock held, caller will release */
			*ep_lock_held = true;
			return dev_cfg;
		}

		/* Address doesn't match this device, release lock and continue */
		pthread_rwlock_unlock(&dev_cfg->endpoint_lock);
	}

	virtio_dev = vnet_pci_device_find_by_bar_addr(address);
	if (virtio_dev) {
		dev_cfg = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + virtio_dev->pf_index];

		/* Acquire read lock before checking device state */
		pthread_rwlock_rdlock(&dev_cfg->endpoint_lock);

		/* Guard against destroyed devices - fallback BAR lookup may match stale addresses
		 * from hot-unplugged devices whose pcie_dev.dw_regs[] weren't cleared. */
		if (!atomic_load(&dev_cfg->device_present) || dev_cfg->tlp_dev == NULL) {
			pthread_rwlock_unlock(&dev_cfg->endpoint_lock);
			DOCA_LOG_DBG("Fallback BAR match for destroyed device at 0x%lx, returning dummy", address);
			return &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
		}

		DOCA_LOG_DBG("Found VirtIO device by BAR address 0x%lx, pf_index %d", address, virtio_dev->pf_index);
		*ep_lock_held = true;
		return dev_cfg;
	}

	/* Return dummy device for invalid addresses */
	return &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
}

#define BDF_MAP_KEY(tlp_type, bdf) (((uint32_t)(tlp_type) << 16) | (bdf))

/*
 * Remove entry from BDF map hash chain
 *
 * @tlp_ctx [in]: TLP context
 * @entry [in]: Entry to remove
 * @old_key [in]: Key value of entry to remove
 */
static void remove_from_bdf_map(struct tlp_context *tlp_ctx, struct bdf_map_entry *entry, uint32_t old_key)
{
	uint32_t old_hash = old_key % BDF_MAP_SIZE;
	struct bdf_map_entry **pp = &tlp_ctx->bdf_map[old_hash];
	while (*pp != NULL) {
		if (*pp == entry) {
			*pp = entry->next;
			entry->next = NULL;
			return;
		}
		pp = &(*pp)->next;
	}
}

/*
 * Invalidate BDF cache entry for an endpoint on a specific bus
 * Called when DSP's secondary_bus changes to prevent stale routing
 *
 * @tlp_ctx [in]: TLP context
 * @old_sec_bus [in]: Old secondary bus number
 * @ep_cfg [in]: Endpoint device configuration
 */
static void invalidate_endpoint_bdf_cache(struct tlp_context *tlp_ctx,
					  uint8_t old_sec_bus,
					  struct pci_device_config *ep_cfg)
{
	uint32_t entry_idx = ep_cfg - tlp_ctx->devs_config;
	struct bdf_map_entry *entry = &tlp_ctx->bdf_entries[entry_idx];

	if (entry->dev_cfg == NULL)
		return;

	/* Endpoint is at device=0, function=0 on DSP's secondary bus */
	uint16_t old_bdf = BDF(old_sec_bus, 0, 0);
	uint32_t old_key_type1 = BDF_MAP_KEY(TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1, old_bdf);

	/* Only remove if the cached key matches the old bus */
	if (entry->key == old_key_type1) {
		DOCA_LOG_INFO("Invalidating BDF cache for EP on old bus 0x%02x", old_sec_bus);
		remove_from_bdf_map(tlp_ctx, entry, entry->key);
		entry->dev_cfg = NULL;
		entry->key = 0;
	}
}

/*
 * Update BDF map with a device entry for fast lookup
 *
 * @tlp_ctx [in]: TLP context
 * @key [in]: BDF map key (includes tlp_type)
 * @dev_cfg [in]: Device pointer
 */
static void update_bdf_map(struct tlp_context *tlp_ctx, uint32_t key, struct pci_device_config *dev_cfg)
{
	uint32_t hash = key % BDF_MAP_SIZE;
	uint32_t entry_idx = dev_cfg - tlp_ctx->devs_config;
	struct bdf_map_entry *entry = &tlp_ctx->bdf_entries[entry_idx];

	/* If entry already mapped with same key, no update needed */
	if (entry->dev_cfg == dev_cfg && entry->key == key)
		return;

	/* If entry was previously mapped with different key, remove from old chain */
	if (entry->dev_cfg != NULL)
		remove_from_bdf_map(tlp_ctx, entry, entry->key);

	/* Add entry to new hash chain */
	entry->key = key;
	entry->dev_cfg = dev_cfg;
	entry->next = tlp_ctx->bdf_map[hash];
	tlp_ctx->bdf_map[hash] = entry;
}

/*
 * Find device by key using hash table for fast lookup
 *
 * @tlp_ctx [in]: TLP context
 * @key [in]: BDF map key (includes tlp_type)
 * @return: Pointer to device or NULL if not found in map
 */
static inline struct pci_device_config *lookup_device_by_key(struct tlp_context *tlp_ctx, uint32_t key)
{
	uint32_t hash = key % BDF_MAP_SIZE;
	struct bdf_map_entry *entry = tlp_ctx->bdf_map[hash];
	while (entry != NULL) {
		if (entry->key == key) {
			if (entry->dev_cfg->is_endpoint && !atomic_load(&entry->dev_cfg->device_present)) {
				return NULL;
			}
			return entry->dev_cfg;
		}
		entry = entry->next;
	}
	return NULL;
}

/*
 * Find device by BDF and TLP type
 *
 * @tlp_ctx [in]: TLP context containing device array
 * @bus [in]: Bus number
 * @device [in]: Device number
 * @function [in]: Function number
 * @tlp_type [in]: TLP request type
 * @return: Pointer to device configuration or dummy device if not found
 */
static struct pci_device_config *find_device_by_bdf(struct tlp_context *tlp_ctx,
						    uint8_t bus,
						    uint8_t device,
						    uint8_t function,
						    uint8_t tlp_type)
{
	uint16_t bdf = BDF(bus, device, function);
	uint32_t key = BDF_MAP_KEY(tlp_type, bdf);
	struct pci_device_config *dev_cfg;
	uint32_t i;

	/* Try fast lookup in BDF map first */
	dev_cfg = lookup_device_by_key(tlp_ctx, key);
	if (dev_cfg != NULL)
		return dev_cfg;

	/* Slow path: find by topology rules and update map */

	/* Type 0 Config Request - USP only */
	if (tlp_type == TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_0) {
		/* USP is accessed via Type 0 (device 0, function 0) */
		if (device == 0 && function == 0) {
			dev_cfg = &tlp_ctx->devs_config[USP_IDX(tlp_ctx)];
			update_bdf_map(tlp_ctx, key, dev_cfg);
			return dev_cfg;
		}
	}
	/* Type 1 Config Request - DSP bridges and endpoints */
	else if (tlp_type == TLP_REQ_TYPE_CONFIG_READ_WRITE_TYPE_1) {
		uint8_t usp_sec_bus = tlp_ctx->devs_config[USP_IDX(tlp_ctx)].cfg_space_hdr.type1.secondary_bus;

		/* Check DSP bridges on USP's secondary bus */
		if (usp_sec_bus != 0 && bus == usp_sec_bus && function == 0) {
			/* DSP devices have device numbers 0-(num_dsp-1) */
			for (i = 0; i < tlp_ctx->num_dsp; i++) {
				if (device == i) {
					dev_cfg = &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i];
					update_bdf_map(tlp_ctx, key, dev_cfg);
					return dev_cfg;
				}
			}
		}

		/* Check Single-PF endpoints on each DSP's secondary bus */
		for (i = 0; i < tlp_ctx->num_ep; i++) {
			struct pci_device_config *ep_cfg = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];
			uint8_t dsp_sec_bus =
				tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i].cfg_space_hdr.type1.secondary_bus;
			if (dsp_sec_bus != 0 && bus == dsp_sec_bus && device == 0 && function == 0 &&
			    atomic_load(&ep_cfg->device_present)) {
				DOCA_LOG_INFO("find_device_by_bdf: matched EP[%u] for bus 0x%02x", i, bus);
				update_bdf_map(tlp_ctx, key, ep_cfg);
				return ep_cfg;
			}
		}
	}

	/* Return dummy device for invalid BDF */
	return &tlp_ctx->devs_config[DUMMY_DEV_IDX(tlp_ctx)];
}

/*
 * Find target device for TLP request (memory or config)
 *
 * For memory requests to endpoints, this function acquires the endpoint's read lock.
 * The caller MUST release the lock after completing the TLP request when *ep_lock_held is true.
 *
 * @tlp_req [in]: TLP request
 * @tlp_ctx [in]: TLP context
 * @tlp_type [in]: TLP request type
 * @ep_lock_held [out]: Set to true if endpoint read lock is held (caller must unlock)
 * @return: Pointer to device configuration (never returns NULL, returns dummy device for invalid requests)
 */
static inline struct pci_device_config *find_target_device(struct doca_devemu_pci_tlp_channel_req *tlp_req,
							   struct tlp_context *tlp_ctx,
							   enum tlp_req_type tlp_type,
							   bool *ep_lock_held)
{
	struct pci_device_config *dev_cfg = NULL;
	const void *req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);

	*ep_lock_held = false;

	if (tlp_type == TLP_REQ_TYPE_MEMORY_READ || tlp_type == TLP_REQ_TYPE_MEMORY_WRITE) {
		uint8_t req_fmt = GET_TLP_REQ_FMT(req_header);
		uint64_t address;
		if (req_fmt == TLP_FMT_4DW_NODATA || req_fmt == TLP_FMT_4DW_W_DATA) {
			uint32_t addr_high = GET_MEM_ADDR_64BIT_HIGH(req_header);
			uint32_t addr_low = GET_MEM_ADDR_64BIT_LOW(req_header);
			address = ((uint64_t)addr_high << 32) | (addr_low << 2);
		} else {
			address = GET_MEM_ADDR_32BIT(req_header);
		}
		dev_cfg = find_device_by_address(tlp_ctx, address, ep_lock_held);
	} else {
		uint8_t bus = GET_TLP_REQ_BUS(req_header);
		uint8_t device = GET_TLP_REQ_DEVICE(req_header);
		uint8_t function = GET_TLP_REQ_FUNCTION(req_header);
		uint8_t req_type = GET_TLP_REQ_TYPE(req_header);
		uint16_t tlp_bdf = BDF(bus, device, function);
		dev_cfg = find_device_by_bdf(tlp_ctx, bus, device, function, req_type);
		/* Set BDF and completer_id on first access, or update if BDF changed (renumbering) */
		if (!dev_cfg->is_bdf_set) {
			dev_cfg->bdf = tlp_bdf;
			dev_cfg->bus = bus;
			dev_cfg->completer_id = tlp_bdf;
			dev_cfg->is_bdf_set = true;
		} else if (dev_cfg->bdf != tlp_bdf) {
			/* BDF changed due to bus renumbering - update completer_id */
			DOCA_LOG_DBG("Device BDF renumbered: 0x%04x -> 0x%04x", dev_cfg->bdf, tlp_bdf);
			dev_cfg->bdf = tlp_bdf;
			dev_cfg->bus = bus;
			dev_cfg->completer_id = tlp_bdf;
		}
	}
	return dev_cfg;
}

/*
 * Read PCI configuration space Type 1 header register 00h (Vendor/Device ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg0(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.device_id << 16) | dev_cfg->cfg_space_hdr.type1.vendor_id;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 01h (Command/Status)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg1(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.status << 16) | dev_cfg->cfg_space_hdr.type1.command;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 02h (Class Code/Revision ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg2(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.class_code << 8) | dev_cfg->cfg_space_hdr.type1.revision_id;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 03h (BIST/Header Type/Latency Timer/Cache Line Size)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg3(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.bist << 24) | (dev_cfg->cfg_space_hdr.type1.header_type << 16) |
		(dev_cfg->cfg_space_hdr.type1.latency_timer << 8) | dev_cfg->cfg_space_hdr.type1.cache_line_size;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 06h (Bus Numbers)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg6(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.secondary_latency << 24) |
		(dev_cfg->cfg_space_hdr.type1.subordinate_bus << 16) |
		(dev_cfg->cfg_space_hdr.type1.secondary_bus << 8) | dev_cfg->cfg_space_hdr.type1.primary_bus;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 07h (I/O Base/Limit)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg7(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.secondary_status << 16) | (dev_cfg->cfg_space_hdr.type1.io_limit << 8) |
		dev_cfg->cfg_space_hdr.type1.io_base;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 08h (Memory Base/Limit)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg8(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.memory_limit << 16) | dev_cfg->cfg_space_hdr.type1.memory_base;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 09h (Prefetchable Memory Base/Limit)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg9(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	/* Include 64-bit capability indicator (bits [3:0] = 0x1 for both base and limit) */
	value = ((dev_cfg->cfg_space_hdr.type1.pre_memory_limit & 0xFFF0) << 16) |
		(dev_cfg->cfg_space_hdr.type1.pre_memory_base & 0xFFF0) | 0x00010001;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 0Ah (Prefetchable Base Upper 32 Bits)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg10(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type1.pre_memory_base_upper_32bit;
}

/*
 * Read PCI configuration space Type 1 header register 0Bh (Prefetchable Limit Upper 32 Bits)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg11(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type1.pre_memory_limit_upper_32bit;
}

/*
 * Read PCI configuration space Type 1 header register 0Ch (I/O Base/Limit Upper 16 Bits)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg12(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.io_limit_upper_16bit << 16) |
		dev_cfg->cfg_space_hdr.type1.io_base_upper_16bit;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 0Dh (Capabilities Pointer)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg13(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = dev_cfg->cfg_space_hdr.type1.cap_ptr;
	return value;
}

/*
 * Read PCI configuration space Type 1 header register 0Eh (Expansion ROM Base Address)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg14(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type1.exp_rom_base_addr;
}

/*
 * Read PCI configuration space Type 1 header register 0Fh (Interrupt Line/Pin/Bridge Control)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type1_read_reg15(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type1.bridge_control << 16) |
		(dev_cfg->cfg_space_hdr.type1.interrupt_pin << 8) | dev_cfg->cfg_space_hdr.type1.interrupt_line;
	return value;
}

/*
 * Write PCI configuration space Type 0 header register 01h (Command/Status)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type0_write_reg1(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg1 {
		uint32_t as_dw;
		struct {
			uint16_t command;
			uint16_t status;
		};
	};
	union reg1 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Handle Command register bits (Read/Write) */
	if (mask.command & COMMAND_RW_MEM_SPACE_ENABLE) {
		if (write_data.command & COMMAND_RW_MEM_SPACE_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_MEM_SPACE_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_MEM_SPACE_ENABLE;
	}
	if (mask.command & COMMAND_RW_BUS_MASTER_ENABLE) {
		if (write_data.command & COMMAND_RW_BUS_MASTER_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_BUS_MASTER_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_BUS_MASTER_ENABLE;
	}
	if (mask.command & COMMAND_RW_PERR_ENABLE) {
		if (write_data.command & COMMAND_RW_PERR_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_PERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_PERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_SERR_ENABLE) {
		if (write_data.command & COMMAND_RW_SERR_ENABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_SERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_SERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_INT_DISABLE) {
		if (write_data.command & COMMAND_RW_INT_DISABLE)
			dev_cfg->cfg_space_hdr.type0.command |= COMMAND_RW_INT_DISABLE;
		else
			dev_cfg->cfg_space_hdr.type0.command &= ~COMMAND_RW_INT_DISABLE;
	}

	/* Handle Status register bits (Write-1-to-Clear) */
	if (mask.status & STATUS_WR1C_MASTER_DATA_PERR) {
		if (write_data.status & STATUS_WR1C_MASTER_DATA_PERR)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_MASTER_DATA_PERR;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_TA) {
		if (write_data.status & STATUS_WR1C_SIGNALED_TA)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_SIGNALED_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_TA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_TA)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_RECEIVE_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_MA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_MA)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_RECEIVE_MA;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_SERR) {
		if (write_data.status & STATUS_WR1C_SIGNALED_SERR)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_SIGNALED_SERR;
	}
	if (mask.status & STATUS_WR1C_DETECTED_PERR) {
		if (write_data.status & STATUS_WR1C_DETECTED_PERR)
			dev_cfg->cfg_space_hdr.type0.status &= ~STATUS_WR1C_DETECTED_PERR;
	}
}

/*
 * Write PCI configuration space Type 0 header BAR registers (04h-09h)
 *
 * @dev_cfg [in/out]: Device configuration
 * @reg_num [in]: Register number (4-9)
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type0_write_bar(struct pci_device_config *dev_cfg,
						uint32_t reg_num,
						uint32_t data,
						uint32_t be_mask)
{
	uint32_t bar_idx;
	union bar_data {
		uint32_t as_dw;
	};
	union bar_data write_data, mask;

	bar_idx = reg_num - 4;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Only support BAR0 (64-bit) and BAR1 (upper 32-bits of BAR0) */
	if (bar_idx > 1)
		return;

	/* BAR sizing: when Host writes all 1s with full mask, return size mask.
	 * Use 32KB BAR to cover doorbell region at offset 0x4000 (matches VNET_VIRTIO_BAR_LOG_SIZE=15). */
	if (write_data.as_dw == 0xFFFFFFFF && mask.as_dw == 0xFFFFFFFF) {
		if (bar_idx == 0) {
			/* BAR0 lower 32-bits: return size mask with type bits */
			dev_cfg->cfg_space_hdr.type0.bar[0] = (~(BAR_SIZE_32K - 1) & 0xFFFFFFFF) |
							      BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT |
							      BAR_MEM_PREFETCHABLE;
			DOCA_LOG_DBG("EP BAR0 Sizing: returning 0x%08x (32KB)", dev_cfg->cfg_space_hdr.type0.bar[0]);
		} else {
			/* BAR1 upper 32-bits: return upper size mask (for 64-bit BAR) */
			dev_cfg->cfg_space_hdr.type0.bar[1] = ((uint64_t) ~(BAR_SIZE_32K - 1) >> 32) & 0xFFFFFFFF;
			DOCA_LOG_DBG("EP BAR1 Sizing: returning 0x%08x (32KB upper)",
				     dev_cfg->cfg_space_hdr.type0.bar[1]);
		}
	} else {
		/* Normal address write: apply masked data while preserving non-written bytes */
		if (mask.as_dw) {
			dev_cfg->cfg_space_hdr.type0.bar[bar_idx] =
				(dev_cfg->cfg_space_hdr.type0.bar[bar_idx] & ~mask.as_dw) |
				(write_data.as_dw & mask.as_dw);
			if (bar_idx == 0) {
				/* BAR0: Clear low 4 bits and set type bits */
				dev_cfg->cfg_space_hdr.type0.bar[0] &= 0xFFFFFFF0;
				dev_cfg->cfg_space_hdr.type0.bar[0] |= BAR_ENCODING_MEM_SPACE | BAR_MEM_TYPE_64_BIT |
								       BAR_MEM_PREFETCHABLE;
			}
			/* Only log complete address when BAR1 (high 32-bits) is written */
			if (bar_idx == 1) {
				uint64_t bar_addr = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
						    (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
				DOCA_LOG_INFO("EP BAR0 Final Address: 0x%lx", bar_addr);
			}
		}
	}
}

/*
 * Handle PCI configuration space header write request (Type 0 - Endpoint)
 *
 * @dev_cfg [in/out]: Device configuration
 * @reg_num [in]: Register number to write
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void handle_type0_config_write(struct pci_device_config *dev_cfg,
					     uint32_t reg_num,
					     uint32_t data,
					     uint32_t be_mask)
{
	switch (reg_num) {
	case 0x0: /* RO */
		break;
	case 0x1:
		config_space_type0_write_reg1(dev_cfg, data, be_mask);
		break;
	case 0x2: /* RO */
		break;
	case 0x3: /* RO */
		break;
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
	case 0x8:
	case 0x9:
		config_space_type0_write_bar(dev_cfg, reg_num, data, be_mask);
		break;
	case 0xa: /* Cardbus CIS Pointer not implemented */
		break;
	case 0xb: /* RO */
		break;
	case 0xc: /* Expansion ROM not implemented */
		break;
	case 0xd: /* RO */
		break;
	case 0xe: /* Reserved */
		break;
	case 0xf: /* Interrupt Line/Pin not implemented */
		break;
	default:
		break;
	}
}

/*
 * Write PCI configuration space Type 1 header register 01h (Command/Status)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg1(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg1 {
		uint32_t as_dw;
		struct {
			uint16_t command;
			uint16_t status;
		};
	};
	union reg1 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Handle Command register bits (Read/Write) */
	if (mask.command & COMMAND_RW_MEM_SPACE_ENABLE) {
		if (write_data.command & COMMAND_RW_MEM_SPACE_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_MEM_SPACE_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_MEM_SPACE_ENABLE;
	}
	if (mask.command & COMMAND_RW_BUS_MASTER_ENABLE) {
		if (write_data.command & COMMAND_RW_BUS_MASTER_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_BUS_MASTER_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_BUS_MASTER_ENABLE;
	}
	if (mask.command & COMMAND_RW_PERR_ENABLE) {
		if (write_data.command & COMMAND_RW_PERR_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_PERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_PERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_SERR_ENABLE) {
		if (write_data.command & COMMAND_RW_SERR_ENABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_SERR_ENABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_SERR_ENABLE;
	}
	if (mask.command & COMMAND_RW_INT_DISABLE) {
		if (write_data.command & COMMAND_RW_INT_DISABLE)
			dev_cfg->cfg_space_hdr.type1.command |= COMMAND_RW_INT_DISABLE;
		else
			dev_cfg->cfg_space_hdr.type1.command &= ~COMMAND_RW_INT_DISABLE;
	}

	/* Handle Status register bits (Write-1-to-Clear) */
	if (mask.status & STATUS_WR1C_MASTER_DATA_PERR) {
		if (write_data.status & STATUS_WR1C_MASTER_DATA_PERR)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_MASTER_DATA_PERR;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_TA) {
		if (write_data.status & STATUS_WR1C_SIGNALED_TA)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_SIGNALED_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_TA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_TA)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_RECEIVE_TA;
	}
	if (mask.status & STATUS_WR1C_RECEIVE_MA) {
		if (write_data.status & STATUS_WR1C_RECEIVE_MA)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_RECEIVE_MA;
	}
	if (mask.status & STATUS_WR1C_SIGNALED_SERR) {
		if (write_data.status & STATUS_WR1C_SIGNALED_SERR)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_SIGNALED_SERR;
	}
	if (mask.status & STATUS_WR1C_DETECTED_PERR) {
		if (write_data.status & STATUS_WR1C_DETECTED_PERR)
			dev_cfg->cfg_space_hdr.type1.status &= ~STATUS_WR1C_DETECTED_PERR;
	}
}

/*
 * Write PCI configuration space Type 1 header register 03h (Cache Line/Latency/Header/BIST)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg3(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg3 {
		uint32_t as_dw;
		struct {
			uint8_t cache_line_size;
			uint8_t latency_timer;
			uint8_t header_type;
			uint8_t bist;
		};
	};
	union reg3 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	if (mask.cache_line_size)
		dev_cfg->cfg_space_hdr.type1.cache_line_size = write_data.cache_line_size;
	if (mask.latency_timer)
		dev_cfg->cfg_space_hdr.type1.latency_timer = write_data.latency_timer;
}

/*
 * Write PCI configuration space Type 1 header register 06h (Bus Numbers)
 * Critical for PCI enumeration - handles primary/secondary/subordinate bus configuration
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @tlp_ctx [in]: TLP context (for device identification in logs)
 */
static inline void config_space_type1_write_reg6(struct pci_device_config *dev_cfg,
						 uint32_t data,
						 uint32_t be_mask,
						 struct tlp_context *tlp_ctx)
{
	union reg6 {
		uint32_t as_dw;
		struct {
			uint8_t primary_bus;
			uint8_t secondary_bus;
			uint8_t subordinate_bus;
			uint8_t secondary_latency_timer;
		};
	};
	union reg6 write_data, mask;
	union reg6 old_val;
	write_data.as_dw = data;
	mask.as_dw = be_mask;
	old_val.primary_bus = dev_cfg->cfg_space_hdr.type1.primary_bus;
	old_val.secondary_bus = dev_cfg->cfg_space_hdr.type1.secondary_bus;
	old_val.subordinate_bus = dev_cfg->cfg_space_hdr.type1.subordinate_bus;

	/*
	 * Handle bus number updates - Accept all values including 0xFF
	 * BIOS uses 0xFF for probe/reset, rejecting it causes enumeration failure
	 */
	if (mask.primary_bus)
		dev_cfg->cfg_space_hdr.type1.primary_bus = write_data.primary_bus;
	if (mask.secondary_bus)
		dev_cfg->cfg_space_hdr.type1.secondary_bus = write_data.secondary_bus;
	if (mask.subordinate_bus)
		dev_cfg->cfg_space_hdr.type1.subordinate_bus = write_data.subordinate_bus;
	if (mask.secondary_latency_timer)
		dev_cfg->cfg_space_hdr.type1.secondary_latency = write_data.secondary_latency_timer;

	/* Identify bridge type for debugging and DSP index for cache invalidation */
	const char *bridge_type = "Unknown";
	int32_t dsp_index = -1;
	if (dev_cfg == &tlp_ctx->devs_config[USP_IDX(tlp_ctx)]) {
		bridge_type = "USP";
	} else {
		for (uint32_t i = 0; i < tlp_ctx->num_dsp; i++) {
			if (dev_cfg == &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i]) {
				dsp_index = (int32_t)i;
				break;
			}
		}
	}

	/* If DSP's secondary_bus changed, invalidate the corresponding endpoint's BDF cache */
	if (dsp_index >= 0 && old_val.secondary_bus != dev_cfg->cfg_space_hdr.type1.secondary_bus &&
	    old_val.secondary_bus != 0) {
		struct pci_device_config *ep_cfg = &tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + dsp_index];
		invalidate_endpoint_bdf_cache(tlp_ctx, old_val.secondary_bus, ep_cfg);
	}

	/* Log significant bus number changes for debugging large topologies */
	if ((old_val.secondary_bus != dev_cfg->cfg_space_hdr.type1.secondary_bus &&
	     dev_cfg->cfg_space_hdr.type1.secondary_bus != 0) ||
	    (old_val.subordinate_bus != dev_cfg->cfg_space_hdr.type1.subordinate_bus &&
	     dev_cfg->cfg_space_hdr.type1.subordinate_bus != 0xFF &&
	     dev_cfg->cfg_space_hdr.type1.subordinate_bus != 0)) {
		if (dsp_index >= 0) {
			DOCA_LOG_INFO("DSP[%u] bus config: P=%02x S=%02x Sub=%02x (was: S=%02x Sub=%02x)",
				      (uint32_t)dsp_index,
				      dev_cfg->cfg_space_hdr.type1.primary_bus,
				      dev_cfg->cfg_space_hdr.type1.secondary_bus,
				      dev_cfg->cfg_space_hdr.type1.subordinate_bus,
				      old_val.secondary_bus,
				      old_val.subordinate_bus);
		} else {
			DOCA_LOG_INFO("%s bus config: P=%02x S=%02x Sub=%02x (was: S=%02x Sub=%02x)",
				      bridge_type,
				      dev_cfg->cfg_space_hdr.type1.primary_bus,
				      dev_cfg->cfg_space_hdr.type1.secondary_bus,
				      dev_cfg->cfg_space_hdr.type1.subordinate_bus,
				      old_val.secondary_bus,
				      old_val.subordinate_bus);
		}
	}
}

/*
 * Write PCI configuration space Type 1 header register 08h (Memory Base/Limit)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg8(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg8 {
		uint32_t as_dw;
		struct {
			uint16_t memory_base;
			uint16_t memory_limit;
		};
	};
	union reg8 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Store address bits (bits [15:4]), lower 4 bits are reserved */
	if (mask.memory_base) {
		dev_cfg->cfg_space_hdr.type1.memory_base = write_data.memory_base & 0xFFF0;
		DOCA_LOG_DBG("Bridge Memory Base Write: 0x%04x", dev_cfg->cfg_space_hdr.type1.memory_base);
	}
	if (mask.memory_limit) {
		dev_cfg->cfg_space_hdr.type1.memory_limit = write_data.memory_limit & 0xFFF0;
		DOCA_LOG_DBG("Bridge Memory Limit Write: 0x%04x", dev_cfg->cfg_space_hdr.type1.memory_limit);
	}
}

/*
 * Write PCI configuration space Type 1 header register 09h (Prefetchable Memory Base/Limit)
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 */
static inline void config_space_type1_write_reg9(struct pci_device_config *dev_cfg, uint32_t data, uint32_t be_mask)
{
	union reg9 {
		uint32_t as_dw;
		struct {
			uint16_t pre_memory_base;
			uint16_t pre_memory_limit;
		};
	};
	union reg9 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	/* Store address bits only, capability bits added during read */
	if (mask.pre_memory_base)
		dev_cfg->cfg_space_hdr.type1.pre_memory_base = write_data.pre_memory_base & 0xFFF0;
	if (mask.pre_memory_limit)
		dev_cfg->cfg_space_hdr.type1.pre_memory_limit = write_data.pre_memory_limit & 0xFFF0;
}

/*
 * Write PCI configuration space Type 1 header register 0Fh (Interrupt/Bridge Control)
 * Critical for BIOS - handles secondary bus reset during host reboot
 * IMPORTANT: Only specific Bridge Control bits are writable (bit 0 and bit 6)
 *            to match hardware behavior and prevent BIOS enumeration errors
 *
 * @dev_cfg [in/out]: Device configuration
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @tlp_ctx [in]: TLP context (for device identification in logs)
 */
static inline void config_space_type1_write_reg15(struct pci_device_config *dev_cfg,
						  uint32_t data,
						  uint32_t be_mask,
						  struct tlp_context *tlp_ctx)
{
	union reg15 {
		uint32_t as_dw;
		struct {
			uint8_t interrupt_line;
			uint8_t interrupt_pin;
			uint16_t bridge_control;
		};
	};
	union reg15 write_data, mask;
	write_data.as_dw = data;
	mask.as_dw = be_mask;

	(void)tlp_ctx;

	if (mask.interrupt_line)
		dev_cfg->cfg_space_hdr.type1.interrupt_line = write_data.interrupt_line;
	if (mask.interrupt_pin)
		dev_cfg->cfg_space_hdr.type1.interrupt_pin = write_data.interrupt_pin;

	/* Only allow writing to specific Bridge Control bits (match tlp_emu behavior)
	 * Only bit 0 (Parity Error Response Enable) and bit 6 (Secondary Bus Reset) are writable
	 */
	if (mask.bridge_control) {
		uint16_t old_value = dev_cfg->cfg_space_hdr.type1.bridge_control;
		uint16_t new_value = old_value;
		uint16_t write_val = write_data.bridge_control;

		/* Update only writable bits: bit 0 and bit 6 */
		new_value = (old_value & ~0x0041) | (write_val & 0x0041);

		/* Log secondary bus reset for debugging */
		if ((write_val & 0x0040) && !(old_value & 0x0040)) {
			DOCA_LOG_DBG("BR_CTL: Secondary Bus Reset 0->1");
		}

		dev_cfg->cfg_space_hdr.type1.bridge_control = new_value;
	}
}

/*
 * Handle PCI configuration space header read request (Type 1)
 *
 * @reg_num [in]: Register number to read
 * @dev_cfg [in]: Device configuration
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_header_read_type1(uint32_t reg_num, struct pci_device_config *dev_cfg)
{
	uint32_t data;

	switch (reg_num) {
	case 0x0:
		data = config_space_type1_read_reg0(dev_cfg);
		break;
	case 0x1:
		data = config_space_type1_read_reg1(dev_cfg);
		break;
	case 0x2:
		data = config_space_type1_read_reg2(dev_cfg);
		break;
	case 0x3:
		data = config_space_type1_read_reg3(dev_cfg);
		break;
	case 0x4:
		data = 0;
		break;
	case 0x5:
		data = 0;
		break;
	case 0x6:
		data = config_space_type1_read_reg6(dev_cfg);
		break;
	case 0x7:
		data = config_space_type1_read_reg7(dev_cfg);
		break;
	case 0x8:
		data = config_space_type1_read_reg8(dev_cfg);
		break;
	case 0x9:
		data = config_space_type1_read_reg9(dev_cfg);
		break;
	case 0xa:
		data = config_space_type1_read_reg10(dev_cfg);
		break;
	case 0xb:
		data = config_space_type1_read_reg11(dev_cfg);
		break;
	case 0xc:
		data = config_space_type1_read_reg12(dev_cfg);
		break;
	case 0xd:
		data = config_space_type1_read_reg13(dev_cfg);
		break;
	case 0xe:
		data = config_space_type1_read_reg14(dev_cfg);
		break;
	case 0xf:
		data = config_space_type1_read_reg15(dev_cfg);
		break;
	default:
		data = 0;
		break;
	}

	return data;
}

/*
 * Read PCI configuration space header register 00h (Vendor/Device ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg0(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.device_id << 16) | dev_cfg->cfg_space_hdr.type0.vendor_id;
	return value;
}

/*
 * Read PCI configuration space header register 01h (Command/Status)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg1(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.status << 16) | dev_cfg->cfg_space_hdr.type0.command;
	return value;
}

/*
 * Read PCI configuration space header register 02h (Class Code/Revision ID)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg2(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.class_code << 8) | dev_cfg->cfg_space_hdr.type0.revision_id;
	return value;
}

/*
 * Read PCI configuration space header register 03h (BIST/Header Type/Latency Timer/Cache Line Size)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg3(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.bist << 24) | (dev_cfg->cfg_space_hdr.type0.header_type << 16) |
		(dev_cfg->cfg_space_hdr.type0.latency_timer << 8) | dev_cfg->cfg_space_hdr.type0.cache_line_size;
	return value;
}

/*
 * Read PCI configuration space BAR register (04h to 09h)
 *
 * @dev_cfg [in]: Device configuration
 * @reg_num [in]: Register number
 * @return: BAR register value
 */
static inline uint32_t config_space_type0_read_bar(struct pci_device_config *dev_cfg, unsigned reg_num)
{
	unsigned bar_id = reg_num - 0x4;
	return dev_cfg->cfg_space_hdr.type0.bar[bar_id];
}

/*
 * Read PCI configuration space header register 0Bh (Subsystem IDs)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg11(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.subsystem_id << 16) | dev_cfg->cfg_space_hdr.type0.subsystem_vendor_id;
	return value;
}

/*
 * Read PCI configuration space header register 0Ch (Expansion ROM Base Address)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg12(struct pci_device_config *dev_cfg)
{
	return dev_cfg->cfg_space_hdr.type0.exp_rom_base_addr;
}

/*
 * Read PCI configuration space header register 0Dh (Capabilities Pointer)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg13(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = dev_cfg->cfg_space_hdr.type0.cap_ptr;
	return value;
}

/*
 * Read PCI configuration space header register 0Fh (Interrupt Line/Pin)
 *
 * @dev_cfg [in]: Device configuration
 * @return: Register value
 */
static inline uint32_t config_space_type0_read_reg15(struct pci_device_config *dev_cfg)
{
	uint32_t value = 0;
	value = (dev_cfg->cfg_space_hdr.type0.max_latency << 24) | (dev_cfg->cfg_space_hdr.type0.min_grant << 16) |
		(dev_cfg->cfg_space_hdr.type0.interrupt_pin << 8) | dev_cfg->cfg_space_hdr.type0.interrupt_line;
	return value;
}

/*
 * Handle PCI configuration space header read request (Type 0)
 *
 * @reg_num [in]: Register number to read
 * @dev_cfg [in]: Device configuration
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_header_read_type0(uint32_t reg_num, struct pci_device_config *dev_cfg)
{
	uint32_t data;

	switch (reg_num) {
	case 0x0:
		data = config_space_type0_read_reg0(dev_cfg);
		break;
	case 0x1:
		data = config_space_type0_read_reg1(dev_cfg);
		break;
	case 0x2:
		data = config_space_type0_read_reg2(dev_cfg);
		break;
	case 0x3:
		data = config_space_type0_read_reg3(dev_cfg);
		break;
	case 0x4 ... 0x9:
		data = config_space_type0_read_bar(dev_cfg, reg_num);
		break;
	case 0xa:
		data = 0;
		break;
	case 0xb:
		data = config_space_type0_read_reg11(dev_cfg);
		break;
	case 0xc:
		data = config_space_type0_read_reg12(dev_cfg);
		break;
	case 0xd:
		data = config_space_type0_read_reg13(dev_cfg);
		break;
	case 0xe:
		data = 0;
		break;
	case 0xf:
		data = config_space_type0_read_reg15(dev_cfg);
		break;
	default:
		data = 0;
		break;
	}

	return data;
}

/*
 * Read PCI Express capability data (matches tlp_handler config_space_cap_pcie_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within Express capability (0-14 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_pcie_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return (dev_cfg->caps.express.pcie_cap_register << 16) | (dev_cfg->caps.express.next_cap_ptr << 8) |
		       dev_cfg->caps.express.cap_id;
	case 1:
		return dev_cfg->caps.express.dev_capabilities;
	case 2:
		return (dev_cfg->caps.express.dev_status << 16) | dev_cfg->caps.express.dev_control;
	case 3:
		return dev_cfg->caps.express.link_capabilities;
	case 4:
		return (dev_cfg->caps.express.link_status << 16) | dev_cfg->caps.express.link_control;
	case 5:
		return dev_cfg->caps.express.slot_capabilities;
	case 6:
		return (dev_cfg->caps.express.slot_status << 16) | dev_cfg->caps.express.slot_control;
	case 7:
		return (dev_cfg->caps.express.root_capabilities << 16) | dev_cfg->caps.express.root_control;
	case 8:
		return dev_cfg->caps.express.root_status;
	case 9:
		return dev_cfg->caps.express.dev_capabilities2;
	case 10:
		return (dev_cfg->caps.express.dev_status2 << 16) | dev_cfg->caps.express.dev_control2;
	case 11:
		return dev_cfg->caps.express.link_capabilities2;
	case 12:
		return (dev_cfg->caps.express.link_status2 << 16) | dev_cfg->caps.express.link_control2;
	case 13:
		return dev_cfg->caps.express.slot_capabilities2;
	case 14:
		return (dev_cfg->caps.express.slot_status2 << 16) | dev_cfg->caps.express.slot_control2;
	default:
		return 0;
	}
}

/*
 * Read VPD capability data (matches tlp_handler config_space_cap_vpd_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within VPD capability (0-1 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_vpd_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return (dev_cfg->caps.vpd.addr_register << 16) | (dev_cfg->caps.vpd.next_cap_ptr << 8) |
		       dev_cfg->caps.vpd.cap_id;
	case 1:
		return dev_cfg->caps.vpd.data_register;
	default:
		return 0;
	}
}

/*
 * Read MSI capability data
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within MSI capability (0-5 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_msi_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0: /* Cap ID + Next + Message Control */
		return (dev_cfg->caps.msi.message_control << 16) | (dev_cfg->caps.msi.next_cap_ptr << 8) |
		       dev_cfg->caps.msi.cap_id;
	case 1: /* Message Address Low */
		return dev_cfg->caps.msi.message_address_low;
	case 2: /* Message Address High */
		return dev_cfg->caps.msi.message_address_high;
	case 3: /* Message Data + Reserved */
		return (dev_cfg->caps.msi.reserved << 16) | dev_cfg->caps.msi.message_data;
	case 4: /* Mask Bits */
		return dev_cfg->caps.msi.mask_bits;
	case 5: /* Pending Bits */
		return dev_cfg->caps.msi.pending_bits;
	default:
		return 0;
	}
}

/*
 * Read MSI-X capability data (matches tlp_handler config_space_cap_msix_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within MSI-X capability (0-2 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_msix_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return (dev_cfg->caps.msix.message_control << 16) | (dev_cfg->caps.msix.next_cap_ptr << 8) |
		       dev_cfg->caps.msix.cap_id;
	case 1:
		return dev_cfg->caps.msix.table_offset;
	case 2:
		return dev_cfg->caps.msix.pba_offset;
	default:
		return 0;
	}
}

/*
 * Read PM capability data (matches tlp_handler config_space_cap_pm_read)
 *
 * @dev_cfg [in]: Device configuration
 * @cap_offset [in]: Offset within PM capability (0-1 DWORDs)
 * @return: Register value
 */
static inline uint32_t config_space_cap_pm_read(struct pci_device_config *dev_cfg, uint32_t cap_offset)
{
	switch (cap_offset) {
	case 0:
		return (dev_cfg->caps.pm.pmc << 16) | (dev_cfg->caps.pm.next_cap_ptr << 8) | dev_cfg->caps.pm.cap_id;
	case 1:
		return (dev_cfg->caps.pm.data << 24) | (dev_cfg->caps.pm.reserved << 16) | dev_cfg->caps.pm.pmcsr;
	default:
		return 0;
	}
}

/*
 * Handle PCI configuration space capabilities read request
 *
 * @reg_num [in]: Register number to read
 * @dev_cfg [in]: Device configuration
 * @cap_id [out]: Pointer to capability ID to be filled
 * @return: Register data value with 1 dword size
 */
static inline uint32_t handle_config_space_caps_read(uint32_t reg_num,
						     struct pci_device_config *dev_cfg,
						     uint16_t *cap_id)
{
	uint32_t cfg_read_data = 0;

	if (reg_num >= TLP_CAP_EXPRESS_REG_NUM && reg_num < TLP_CAP_EXPRESS_REG_NUM + TLP_CAP_EXPRESS_LEN_DW) {
		cfg_read_data = config_space_cap_pcie_read(dev_cfg, reg_num - TLP_CAP_EXPRESS_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_EXPRESS;
	} else if (reg_num >= TLP_CAP_VPD_REG_NUM && reg_num < TLP_CAP_VPD_REG_NUM + TLP_CAP_VPD_LEN_DW) {
		cfg_read_data = config_space_cap_vpd_read(dev_cfg, reg_num - TLP_CAP_VPD_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_VPD;
	} else if (reg_num >= TLP_CAP_MSIX_REG_NUM && reg_num < TLP_CAP_MSIX_REG_NUM + TLP_CAP_MSIX_LEN_DW) {
		cfg_read_data = config_space_cap_msix_read(dev_cfg, reg_num - TLP_CAP_MSIX_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_MSIX;
	} else if (reg_num >= TLP_CAP_MSI_REG_NUM && reg_num < TLP_CAP_MSI_REG_NUM + TLP_CAP_MSI_LEN_DW) {
		cfg_read_data = config_space_cap_msi_read(dev_cfg, reg_num - TLP_CAP_MSI_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_MSI;
	} else if (reg_num >= TLP_CAP_PM_REG_NUM && reg_num < TLP_CAP_PM_REG_NUM + TLP_CAP_PM_LEN_DW) {
		cfg_read_data = config_space_cap_pm_read(dev_cfg, reg_num - TLP_CAP_PM_REG_NUM);
		*cap_id = TLP_PCI_CAP_ID_PM;
	} else {
		cfg_read_data = 0x00000000;
	}

	return cfg_read_data;
}

/**
 * Set TLP request completion header context
 *
 * All completion fields are passed as parameters or extracted from tlp_req directly,
 * eliminating shared-state race conditions with concurrent TLP requests.
 *
 * @tlp_response_header [out]: Pointer to completion header buffer
 * @tlp_req [in]: Original TLP request (used to extract requester_id, tag, and optionally BDF)
 * @cmpl_fmt [in]: Completion format (e.g. TLP_FMT_CPL_W_DATA, TLP_FMT_CPL_NODATA)
 * @cmpl_status [in]: Completion status (e.g. TLP_CPL_STATUS_SC, TLP_CPL_STATUS_UR)
 * @cmpl_length [in]: Completion data length in DWORDs
 * @byte_count [in]: Byte count for the completion header
 * @completer_id_override [in]: If non-zero, use as completer_id (required for memory TLPs
 *                              where DW2 contains address, not BDF).
 *                              If zero, extract BDF from DW2 (only valid for config TLPs).
 */
static inline void set_tlp_req_completion_header(void *tlp_response_header,
						 struct doca_devemu_pci_tlp_channel_req *tlp_req,
						 uint8_t cmpl_fmt,
						 uint8_t cmpl_status,
						 uint8_t cmpl_length,
						 uint16_t byte_count,
						 uint16_t completer_id_override)
{
	uint32_t *header_dw = (uint32_t *)tlp_response_header;

	/* Extract completion-related values directly from the TLP request header.
	 * This ensures we use the correct values for THIS specific request,
	 * avoiding race conditions with concurrent requests. */
	const void *req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint16_t requester_id = GET_TLP_REQ_REQ_ID(req_header);
	uint8_t tag = GET_TLP_REQ_TAG(req_header);
	uint8_t tag9 = GET_TLP_REQ_TAG9(req_header);
	uint8_t tag8 = GET_TLP_REQ_TAG8(req_header);

	uint16_t completer_id;
	if (completer_id_override != 0) {
		/* Use override for memory TLPs (DW2 contains address, not BDF) */
		completer_id = completer_id_override;
	} else {
		/* Extract from DW2 for config TLPs (contains BDF of target device) */
		uint8_t bus = GET_TLP_REQ_BUS(req_header);
		uint8_t device = GET_TLP_REQ_DEVICE(req_header);
		uint8_t function = GET_TLP_REQ_FUNCTION(req_header);
		completer_id = (bus << 8) | (device << 3) | function;
	}

	memset(tlp_response_header, 0, 3 * sizeof(uint32_t));

	/* DW0: fmt[31:29], type[28:24], tag9[bit 23], tag8[bit 19], length[9:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 29), cmpl_fmt, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(28, 24), TLP_TYPE_COMPLETION, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(23, 23), tag9, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(19, 19), tag8, &header_dw[0]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(9, 0), cmpl_length, &header_dw[0]);

	/* DW1: completer_id[31:16], cmpl_status[15:13], byte_cnt[11:0] */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), completer_id, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 13), cmpl_status, &header_dw[1]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(11, 0), byte_count, &header_dw[1]);

	/* DW2: requester_id[31:16], tag[15:8], lower_addr[6:0] (set to 0) */
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(31, 16), requester_id, &header_dw[2]);
	DOCA_BE32P_SET(DOCA_BE32_GENMASK(15, 8), tag, &header_dw[2]);
	/* lower_addr are already zeroed by memset */
}

/*
 * Handle TLP request read type_0
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_read_type_0(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct pci_device_config *dev_cfg,
					      struct tlp_context *tlp_ctx,
					      uint32_t ext_reg_num,
					      uint16_t *cap_id,
					      bool *is_cap_id_valid,
					      bool *is_pcie_cap)
{
	uint32_t cfg_read_data = 0;
	(void)tlp_ctx;

	// DOCA_LOG_INFO("Handle TLP Request for Config Read Type 0, ext_reg_num: %d is_endpoint: %d", ext_reg_num,
	// dev_cfg->is_endpoint);

	if (dev_cfg->is_endpoint) {
		DOCA_LOG_INFO("Handle TLP Request for Config Read Type 0, ext_reg_num: %d, is_endpoint: %d",
			      ext_reg_num,
			      dev_cfg->is_endpoint);
		struct vnet_pci_device *virtio_dev = vnet_pci_device_find_by_tlp_cfg(tlp_req);
		if (!virtio_dev) {
			virtio_dev = vnet_pci_device_enumerate(tlp_ctx, tlp_req, dev_cfg);
			if (!virtio_dev) {
				/* Endpoint enumeration failed - send UR completion */
				DOCA_LOG_ERR("Endpoint enumeration failed for config read type 0");
				vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_UR, 0, 4);
				doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
				return;
			}
		}
		vnet_pci_dev_handle_cfg_read0(tlp_req, tlp_ctx);
		return;
	}

	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			cfg_read_data = handle_config_space_header_read_type1(ext_reg_num, dev_cfg);
		else
			cfg_read_data = handle_config_space_header_read_type0(ext_reg_num, dev_cfg);
		break;
	case 16 ... 63:
		/* Capabilities - for endpoints and bridges (DSP with hotplug) */
		cfg_read_data = handle_config_space_caps_read(ext_reg_num, dev_cfg, cap_id);
		*is_cap_id_valid = true;
		*is_pcie_cap = false;
		break;
	default:
		DOCA_LOG_DBG("Invalid read ext_reg_num %d", ext_reg_num);
		break;
	}
	/* Bridge/non-endpoint path - send successful completion with config data */
	DOCA_LOG_DBG(" --> cfg_read_data:  %08x", cfg_read_data);
	void *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(tlp_cpl_data, &cfg_read_data, sizeof(cfg_read_data));
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      tlp_req,
				      TLP_FMT_CPL_W_DATA,
				      TLP_CPL_STATUS_SC,
				      1,
				      BYTES_IN_DWORD,
				      0);
	doca_devemu_pci_tlp_channel_req_complete_config_read(tlp_req,
							     dev_cfg->tlp_dev,
							     (uint8_t)*is_cap_id_valid,
							     *cap_id,
							     (uint8_t)*is_pcie_cap);
	(void)tlp_ctx;
}

/*
 * Handle TLP request read type_1
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_read_type_1(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct pci_device_config *dev_cfg,
					      struct tlp_context *tlp_ctx,
					      uint32_t ext_reg_num,
					      uint16_t *cap_id,
					      bool *is_cap_id_valid,
					      bool *is_pcie_cap)
{
	uint32_t cfg_read_data = 0;
	(void)tlp_ctx;

	DOCA_LOG_DBG("Handle TLP Request for Config Read Type 1, ext_reg_num: %d", ext_reg_num);

	if (dev_cfg->is_endpoint) {
		struct vnet_pci_device *virtio_dev = vnet_pci_device_find_by_tlp_cfg(tlp_req);
		if (!virtio_dev) {
			virtio_dev = vnet_pci_device_enumerate(tlp_ctx, tlp_req, dev_cfg);
			if (!virtio_dev) {
				/* Endpoint enumeration failed - send UR completion */
				DOCA_LOG_ERR("Endpoint enumeration failed for config read type 1");
				vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_UR, 0, 4);
				doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
				return;
			}
		}
		vnet_pci_dev_handle_cfg_read0(tlp_req, tlp_ctx);
		return;
	}

	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			cfg_read_data = handle_config_space_header_read_type1(ext_reg_num, dev_cfg);
		else
			cfg_read_data = handle_config_space_header_read_type0(ext_reg_num, dev_cfg);
		break;
	case 16 ... 63:
		/* Capabilities - for endpoints and bridges (DSP with hotplug) */
		cfg_read_data = handle_config_space_caps_read(ext_reg_num, dev_cfg, cap_id);
		*is_cap_id_valid = true;
		*is_pcie_cap = false;
		break;
	default:
		DOCA_LOG_DBG("Invalid read ext_reg_num %d", ext_reg_num);
		break;
	}
	/* Bridge/non-endpoint path - send successful completion with config data */
	DOCA_LOG_DBG(" --> cfg_read_data:  %08x", cfg_read_data);
	void *tlp_cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	memcpy(tlp_cpl_data, &cfg_read_data, sizeof(cfg_read_data));
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      tlp_req,
				      TLP_FMT_CPL_W_DATA,
				      TLP_CPL_STATUS_SC,
				      1,
				      BYTES_IN_DWORD,
				      0);
	doca_devemu_pci_tlp_channel_req_complete_config_read(tlp_req,
							     dev_cfg->tlp_dev,
							     (uint8_t)*is_cap_id_valid,
							     *cap_id,
							     (uint8_t)*is_pcie_cap);
	(void)tlp_ctx;
}

/*
 * Map TLP first DW byte enable to byte mask
 * Matches dpu sample (devemu_pci_device_tlp_handler) implementation
 *
 * @first_dw_be [in]: First DW byte enable (4 bits)
 * @return: Corresponding byte mask based on PCI specification
 */
static inline uint32_t map_first_dw_be_to_mask(uint32_t first_dw_be)
{
	static const uint32_t first_dw_be_to_mask[] = {
		0x00000000, /* 0000 */
		0x000000FF, /* 0001 */
		0x0000FF00, /* 0010 */
		0x0000FFFF, /* 0011 */
		0x00FF0000, /* 0100 */
		0x00FF00FF, /* 0101 */
		0x00FFFF00, /* 0110 */
		0x00FFFFFF, /* 0111 */
		0xFF000000, /* 1000 */
		0xFF0000FF, /* 1001 */
		0xFF00FF00, /* 1010 */
		0xFF00FFFF, /* 1011 */
		0xFFFF0000, /* 1100 */
		0xFFFF00FF, /* 1101 */
		0xFFFFFF00, /* 1110 */
		0xFFFFFFFF, /* 1111 */
	};

	return first_dw_be_to_mask[first_dw_be & 0xF];
}

/*
 * Handle PCI configuration space header write request (Type 1 - Bridge)
 *
 * @dev_cfg [in/out]: Device configuration
 * @reg_num [in]: Register number to write
 * @data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @tlp_ctx [in]: TLP context
 */
static inline void handle_type1_config_write(struct pci_device_config *dev_cfg,
					     uint32_t reg_num,
					     uint32_t data,
					     uint32_t be_mask,
					     struct tlp_context *tlp_ctx)
{
	switch (reg_num) {
	case 0x0: /* RO */
		break;
	case 0x1:
		config_space_type1_write_reg1(dev_cfg, data, be_mask);
		break;
	case 0x2: /* RO */
		break;
	case 0x3:
		config_space_type1_write_reg3(dev_cfg, data, be_mask);
		break;
	case 0x4: /* BAR0 not implemented */
		break;
	case 0x5: /* BAR1 not implemented */
		break;
	case 0x6:
		config_space_type1_write_reg6(dev_cfg, data, be_mask, tlp_ctx);
		break;
	case 0x7: /* I/O Base/Limit not implemented */
		break;
	case 0x8:
		config_space_type1_write_reg8(dev_cfg, data, be_mask);
		break;
	case 0x9:
		config_space_type1_write_reg9(dev_cfg, data, be_mask);
		break;
	case 0xa:
		if (be_mask)
			dev_cfg->cfg_space_hdr.type1.pre_memory_base_upper_32bit = data;
		break;
	case 0xb:
		if (be_mask)
			dev_cfg->cfg_space_hdr.type1.pre_memory_limit_upper_32bit = data;
		break;
	case 0xc: /* I/O Base/Limit Upper not implemented */
		break;
	case 0xd: /* RO */
		break;
	case 0xe: /* Expansion ROM not implemented */
		break;
	case 0xf:
		config_space_type1_write_reg15(dev_cfg, data, be_mask, tlp_ctx);
		break;
	default:
		break;
	}
}

/*
 * Write MSI capability data
 *
 * @cap_offset [in]: Offset within MSI capability (0-5 DWORDs)
 * @write_data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @dev_cfg [in/out]: Device configuration
 */
static inline void config_space_cap_msi_write(uint32_t cap_offset,
					      uint32_t write_data,
					      uint32_t be_mask,
					      struct pci_device_config *dev_cfg)
{
	switch (cap_offset) {
	case 0: /* Message Control */
		if (be_mask & 0xFFFF0000) {
			dev_cfg->caps.msi.message_control = (write_data >> 16) & 0xFFFF;
			DOCA_LOG_DBG("MSI Control: 0x%04X", dev_cfg->caps.msi.message_control);
		}
		break;
	case 1: /* Message Address Low */
		dev_cfg->caps.msi.message_address_low = write_data;
		DOCA_LOG_DBG("MSI_ADDR_LOW: 0x%08X", write_data);
		break;
	case 2: /* Message Address High */
		dev_cfg->caps.msi.message_address_high = write_data;
		DOCA_LOG_DBG("MSI_ADDR_HIGH: 0x%08X", write_data);
		DOCA_LOG_DBG("MSI_FULL_ADDR: 0x%016lX",
			     ((uint64_t)dev_cfg->caps.msi.message_address_high << 32) |
				     dev_cfg->caps.msi.message_address_low);
		break;
	case 3: /* Message Data (lower 16 bits) + Reserved (upper 16 bits) */
		DOCA_LOG_DBG("MSI_DATA_RAW: write_data=0x%08X, be_mask=0x%02X", write_data, be_mask);
		if (be_mask & 0x03) { /* Bytes 0-1 = Message Data */
			dev_cfg->caps.msi.message_data = write_data & 0xFFFF;
			DOCA_LOG_DBG("MSI_DATA: 0x%04X (vector=%u)",
				     dev_cfg->caps.msi.message_data,
				     dev_cfg->caps.msi.message_data & 0xFF);
		}
		break;
	case 4: /* Mask Bits */
		dev_cfg->caps.msi.mask_bits = write_data;
		DOCA_LOG_INFO("MSI Mask Bits: 0x%08X", dev_cfg->caps.msi.mask_bits);
		break;
	case 5: /* Pending Bits (read-only) */
		/* Pending bits are read-only, no write operation */
		break;
	default:
		break;
	}
}

/*
 * Trigger Command Completed event and send MSI if enabled
 *
 * @tlp_ctx [in]: TLP context
 * @dsp [in/out]: DSP device configuration
 * @return: DOCA_SUCCESS on success
 */
static doca_error_t trigger_command_completed(struct tlp_context *tlp_ctx, struct pci_device_config *dsp)
{
	/* Set Command Completed status bit */
	dsp->caps.express.slot_status |= SLOT_STS_CMD_COMPLETED;

	DOCA_LOG_DBG("Command Completed: control=0x%04X, status=0x%04X",
		     dsp->caps.express.slot_control,
		     dsp->caps.express.slot_status);

	/* Send MSI if Command Completed Interrupt is enabled */
	if (dsp->caps.express.slot_control & SLOT_CTRL_CMD_COMPLETED_INT_EN) {
		doca_error_t result = send_msi_via_memory_write_tlp(tlp_ctx, dsp);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_DBG("Failed to send Command Completed MSI: %s", doca_error_get_descr(result));
			return result;
		}
		DOCA_LOG_INFO("Command Completed MSI sent");
	}

	return DOCA_SUCCESS;
}

/*
 * Write PCIe Express capability data
 *
 * @cap_offset [in]: Offset within Express capability (0-15 DWORDs)
 * @write_data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @dev_cfg [in/out]: Device configuration to update
 * @tlp_ctx [in]: TLP context (needed to submit delayed_destroy on Power OFF)
 * @return: True if Command Completed should be triggered, false otherwise
 */
static inline bool config_space_cap_pcie_write(uint32_t cap_offset,
					       uint32_t write_data,
					       uint32_t be_mask,
					       struct pci_device_config *dev_cfg,
					       struct tlp_context *tlp_ctx)
{
	bool trigger_cmd_completed = false;

	switch (cap_offset) {
	case 6: /* Slot Control + Slot Status (Offset 0x18 in Express Cap) */
		/* Handle Slot Control (lower 16 bits) */
		if (be_mask & 0x0000FFFF) {
			uint16_t old_control = dev_cfg->caps.express.slot_control;
			uint16_t new_control = write_data & 0xFFFF;

			/* Host sets enable bits via pcie_enable_notification(), accept as-is */
			dev_cfg->caps.express.slot_control = new_control;

			if (dev_cfg->is_bridge) {
				/* Check if device is present (PDS bit in Slot Status) */
				bool device_present =
					(dev_cfg->caps.express.slot_status & SLOT_STS_PRESENCE_DETECT_STATE) != 0;

				/*
				 * Detect Power Controller Control state change (Bit 10)
				 * PCIe Spec: bit10=0 means Power ON, bit10=1 means Power OFF
				 */
				bool old_power_off = (old_control & SLOT_CTRL_POWER_CONTROLLER) != 0;
				bool new_power_off = (new_control & SLOT_CTRL_POWER_CONTROLLER) != 0;

				if (old_power_off && !new_power_off && device_present) {
					/* Power OFF -> ON with device present: activate Data Link Layer.
					 * RELEASE store pairs with the ACQUIRE load on the worker
					 * thread (PE2) in trigger_hotplug_event() unplug-defer wait,
					 * giving a well-defined happens-before relationship under
					 * the C11 memory model. */
					__atomic_or_fetch(&dev_cfg->caps.express.link_status,
							  LINK_STS_DL_ACTIVE,
							  __ATOMIC_RELEASE);
					DOCA_LOG_INFO("Power ON (bit10: 1->0): DLActive set, link_status=0x%04X",
						      dev_cfg->caps.express.link_status);
				} else if (!old_power_off && new_power_off) {
					/* Power ON -> OFF: deactivate Data Link Layer.
					 * RELEASE store pairs with the ACQUIRE load on PE2 (see
					 * Power ON branch above). */
					__atomic_and_fetch(&dev_cfg->caps.express.link_status,
							   (uint16_t)~LINK_STS_DL_ACTIVE,
							   __ATOMIC_RELEASE);
					DOCA_LOG_INFO("Power OFF (bit10: 0->1): DLActive cleared, link_status=0x%04X",
						      dev_cfg->caps.express.link_status);

					/*
					 * Trigger endpoint destroy ONLY on this real 1->0 transition.
					 * Previously this was done unconditionally whenever any cap
					 * write arrived with DLActive=0, which incorrectly fired for
					 * freshly-plugged DSPs whose DLActive had not yet been set by
					 * the host's Power ON, leading to premature destruction.
					 */
					if (tlp_ctx != NULL) {
						for (uint32_t i = 0; i < tlp_ctx->num_dsp; i++) {
							if (dev_cfg !=
							    &tlp_ctx->devs_config[FIRST_DSP_IDX(tlp_ctx) + i])
								continue;
							struct pci_device_config *ep =
								&tlp_ctx->devs_config[FIRST_PF_IDX(tlp_ctx) + i];

							if (atomic_load(&ep->pending_unplug) &&
							    atomic_load(&ep->device_present)) {
								DOCA_LOG_INFO(
									"DSP[%u] power off (DLActive=0) - triggering endpoint destroy",
									i);
								pci_cfg_workqueue_submit_delayed_destroy(tlp_ctx, ep);
							}
							break;
						}
					}
				}

				/* Trigger Command Completed for any Slot Control write */
				trigger_cmd_completed = true;
			}
		}

		/* Handle Slot Status (upper 16 bits) - Write-1-to-Clear bits */
		if (be_mask & 0xFFFF0000) {
			uint16_t status_w1c = (write_data >> 16) & 0xFFFF;
			/* Clear bits that are written as 1 (W1C behavior) */
			dev_cfg->caps.express.slot_status &= ~status_w1c;
			DOCA_LOG_DBG("Slot Status W1C: cleared bits 0x%04X, new status=0x%04X",
				     status_w1c,
				     dev_cfg->caps.express.slot_status);
		}
		break;

	case 2: /* Device Control + Device Status */
		/* Handle Device Control (lower 16 bits) - if needed */
		if (be_mask & 0x0000FFFF) {
			dev_cfg->caps.express.dev_control = write_data & 0xFFFF;
			DOCA_LOG_DBG("Device Control write: 0x%04X", dev_cfg->caps.express.dev_control);
		}
		/* Device Status (upper 16 bits) is mostly RO or W1C */
		break;

	case 4: /* Link Control + Link Status */
		/* Handle Link Control (lower 16 bits) - if needed */
		if (be_mask & 0x0000FFFF) {
			dev_cfg->caps.express.link_control = write_data & 0xFFFF;
			DOCA_LOG_DBG("Link Control write: 0x%04X", dev_cfg->caps.express.link_control);
		}
		/* Link Status (upper 16 bits) is mostly RO */
		break;

	default:
		/* Other Express capability registers are mostly RO */
		DOCA_LOG_DBG("Express cap write to offset %u (ignored)", cap_offset);
		break;
	}

	return trigger_cmd_completed;
}

/*
 * Handle PCI configuration space capabilities write request
 *
 * @reg_num [in]: Register number
 * @cfg_write_data [in]: Data to write
 * @be_mask [in]: Byte enable mask
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context (forwarded to pcie cap handler for Power OFF destroy)
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 * @return: True if Command Completed should be triggered, false otherwise
 */
static inline bool handle_config_space_caps_write(uint32_t reg_num,
						  uint32_t cfg_write_data,
						  uint32_t be_mask,
						  struct pci_device_config *dev_cfg,
						  struct tlp_context *tlp_ctx,
						  uint16_t *cap_id,
						  bool *is_pcie_cap)
{
	bool trigger_cmd_completed = false;
	/* Determine capability ID based on offset and perform write */
	if (reg_num >= TLP_CAP_EXPRESS_REG_NUM && reg_num < TLP_CAP_EXPRESS_REG_NUM + TLP_CAP_EXPRESS_LEN_DW) {
		trigger_cmd_completed = config_space_cap_pcie_write(reg_num - TLP_CAP_EXPRESS_REG_NUM,
								    cfg_write_data,
								    be_mask,
								    dev_cfg,
								    tlp_ctx);
		*cap_id = TLP_PCI_CAP_ID_EXPRESS;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_VPD_REG_NUM && reg_num < TLP_CAP_VPD_REG_NUM + TLP_CAP_VPD_LEN_DW) {
		*cap_id = TLP_PCI_CAP_ID_VPD;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_MSIX_REG_NUM && reg_num < TLP_CAP_MSIX_REG_NUM + TLP_CAP_MSIX_LEN_DW) {
		*cap_id = TLP_PCI_CAP_ID_MSIX;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_MSI_REG_NUM && reg_num < TLP_CAP_MSI_REG_NUM + TLP_CAP_MSI_LEN_DW) {
		config_space_cap_msi_write(reg_num - TLP_CAP_MSI_REG_NUM, cfg_write_data, be_mask, dev_cfg);
		*cap_id = TLP_PCI_CAP_ID_MSI;
		*is_pcie_cap = false;
	} else if (reg_num >= TLP_CAP_PM_REG_NUM && reg_num < TLP_CAP_PM_REG_NUM + TLP_CAP_PM_LEN_DW) {
		*cap_id = TLP_PCI_CAP_ID_PM;
		*is_pcie_cap = false;
	}

	return trigger_cmd_completed;
}

/*
 * Handle TLP request write type_0
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @tlp_req_header [in]: Pointer to TLP request header
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_write_type_0(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					       struct pci_device_config *dev_cfg,
					       struct tlp_context *tlp_ctx,
					       uint32_t ext_reg_num,
					       const void *tlp_req_header,
					       uint16_t *cap_id,
					       bool *is_cap_id_valid,
					       bool *is_pcie_cap)
{
	unsigned be_mask = map_first_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	const uint32_t *cfg_write_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);
	uint32_t write_data = 0;

	if (cfg_write_data != NULL)
		write_data = cfg_write_data[0];

	// DOCA_LOG_INFO("Handle TLP Request for Config Write Type 0, ext_reg_num: %d is_endpoint: %d", ext_reg_num,
	// dev_cfg->is_endpoint); DOCA_LOG_INFO(" --> cfg_write_data: %08x, be_mask: %08x", write_data, be_mask);

	bool trigger_cmd_completed = false;

	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			handle_type1_config_write(dev_cfg, ext_reg_num, write_data, be_mask, tlp_ctx);
		else
			handle_type0_config_write(dev_cfg, ext_reg_num, write_data, be_mask);
		break;
	case 16 ... 63:
		/* Capabilities - for endpoints and bridges (DSP with hotplug) */
		trigger_cmd_completed = handle_config_space_caps_write(ext_reg_num,
								       write_data,
								       be_mask,
								       dev_cfg,
								       tlp_ctx,
								       cap_id,
								       is_pcie_cap);
		*is_cap_id_valid = true;
		break;
	default:
		DOCA_LOG_DBG("Invalid write ext_reg_num %d", ext_reg_num);
		break;
	}

	if (dev_cfg->is_endpoint) {
		DOCA_LOG_INFO("Handle TLP Request for Config Write Type 0, ext_reg_num: %d, is_endpoint: %d",
			      ext_reg_num,
			      dev_cfg->is_endpoint);
		DOCA_LOG_INFO(" --> cfg_write_data: %08x, be_mask: %08x", write_data, be_mask);
		struct vnet_pci_device *virtio_dev = vnet_pci_device_find_by_tlp_cfg(tlp_req);
		if (!virtio_dev) {
			virtio_dev = vnet_pci_device_enumerate(tlp_ctx, tlp_req, dev_cfg);
			if (!virtio_dev) {
				/* Endpoint enumeration failed - send UR completion */
				DOCA_LOG_ERR("Endpoint enumeration failed for config write type 0");
				vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_UR, 0, 4);
				doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
				return;
			}
		}
		vnet_pci_dev_handle_cfg_write0(tlp_req, tlp_ctx);
		return;
	}
	/* Bridge/non-endpoint path - send successful completion */
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      tlp_req,
				      TLP_FMT_CPL_NODATA,
				      TLP_CPL_STATUS_SC,
				      0,
				      BYTES_IN_DWORD,
				      0);
	doca_devemu_pci_tlp_channel_req_complete_config_write(tlp_req,
							      dev_cfg->tlp_dev,
							      (uint8_t)*is_cap_id_valid,
							      *cap_id,
							      (uint8_t)*is_pcie_cap);

	/* Trigger Command Completed if needed (for Slot Control writes) */
	if (trigger_cmd_completed)
		trigger_command_completed(tlp_ctx, dev_cfg);

	(void)tlp_ctx;
}

/*
 * Handle TLP request write type_1
 *
 * @tlp_req [in]: Pointer to TLP request
 * @dev_cfg [in]: Device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_dev [in]: TLP device
 * @ext_reg_num [in]: ext reg num
 * @tlp_req_header [in]: Pointer to TLP request header
 * @cap_id [out]: Pointer to capability ID to be filled
 * @is_cap_id_valid [out]: Pointer to is_cap_id_valid to be filled
 * @is_pcie_cap [out]: Pointer to is_pcie_cap to be filled
 */
static inline void handle_tlp_req_write_type_1(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					       struct pci_device_config *dev_cfg,
					       struct tlp_context *tlp_ctx,
					       uint32_t ext_reg_num,
					       const void *tlp_req_header,
					       uint16_t *cap_id,
					       bool *is_cap_id_valid,
					       bool *is_pcie_cap)
{
	unsigned be_mask = map_first_dw_be_to_mask(GET_TLP_REQ_FIRST_DW_BE(tlp_req_header));
	const uint32_t *cfg_write_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);
	uint32_t write_data = 0;

	if (cfg_write_data != NULL)
		write_data = cfg_write_data[0];

	DOCA_LOG_DBG("Handle TLP Request for Config Write Type 1, ext_reg_num: %d", ext_reg_num);
	DOCA_LOG_DBG(" --> cfg_write_data: %08x, be_mask: %08x", write_data, be_mask);

	bool trigger_cmd_completed = false;

	switch (ext_reg_num) {
	case 0 ... 15:
		/* Header region - depends on device type */
		if (dev_cfg->is_bridge)
			handle_type1_config_write(dev_cfg, ext_reg_num, write_data, be_mask, tlp_ctx);
		else
			handle_type0_config_write(dev_cfg, ext_reg_num, write_data, be_mask);
		break;
	case 16 ... 63:
		/*
		 * Capabilities - for endpoints and bridges (DSP with hotplug).
		 * Power OFF detection and delayed_destroy submission for the DSP slot
		 * are now handled inside config_space_cap_pcie_write() on the actual
		 * SLOT_CTRL bit10 0->1 transition, so we no longer inspect DLActive
		 * here (which incorrectly fired on every cap write for freshly-plugged
		 * DSPs whose Power ON had not yet landed).
		 */
		trigger_cmd_completed = handle_config_space_caps_write(ext_reg_num,
								       write_data,
								       be_mask,
								       dev_cfg,
								       tlp_ctx,
								       cap_id,
								       is_pcie_cap);
		*is_cap_id_valid = true;
		break;
	default:
		DOCA_LOG_DBG("Invalid write ext_reg_num %d", ext_reg_num);
		break;
	}

	if (dev_cfg->is_endpoint) {
		struct vnet_pci_device *virtio_dev = vnet_pci_device_find_by_tlp_cfg(tlp_req);
		if (!virtio_dev) {
			virtio_dev = vnet_pci_device_enumerate(tlp_ctx, tlp_req, dev_cfg);
			if (!virtio_dev) {
				/* Endpoint enumeration failed - send UR completion */
				DOCA_LOG_ERR("Endpoint enumeration failed for config write type 1");
				vnet_pci_dev_tlp_cfg_cpl_prep(tlp_req, TLP_FMT_3DW_NODATA, TLP_CPL_STATUS_UR, 0, 4);
				doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
				return;
			}
		}
		vnet_pci_dev_handle_cfg_write0(tlp_req, tlp_ctx);
		return;
	}
	/* Bridge/non-endpoint path - send successful completion */
	set_tlp_req_completion_header(doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req),
				      tlp_req,
				      TLP_FMT_CPL_NODATA,
				      TLP_CPL_STATUS_SC,
				      0,
				      BYTES_IN_DWORD,
				      0);
	doca_devemu_pci_tlp_channel_req_complete_config_write(tlp_req,
							      dev_cfg->tlp_dev,
							      (uint8_t)*is_cap_id_valid,
							      *cap_id,
							      (uint8_t)*is_pcie_cap);

	/* Trigger Command Completed if needed (for Slot Control writes) */
	if (trigger_cmd_completed)
		trigger_command_completed(tlp_ctx, dev_cfg);

	(void)tlp_ctx;
}

/*
 * Helper function to determine byte enable mask for a specific DWORD
 *
 * @dw_idx [in]: DWORD index
 * @num_dwords [in]: Total number of DWORDs
 * @first_dw_be [in]: First DWORD byte enable
 * @last_dw_be [in]: Last DWORD byte enable
 * @return: Byte enable mask for the specified DWORD
 */
static inline unsigned get_dword_byte_enable_mask(unsigned dw_idx,
						  unsigned num_dwords,
						  unsigned first_dw_be,
						  unsigned last_dw_be)
{
	if (num_dwords == 1 || dw_idx == 0) {
		return first_dw_be;
	} else if (dw_idx == num_dwords - 1) {
		return last_dw_be;
	} else {
		return 0xF; /* All bytes enabled */
	}
}

/*
 * Handle TLP memory read request (MMIO Read)
 * Returns dummy data for testing purposes
 *
 * @tlp_req [in]: TLP request
 * @dev_cfg [in]: Target device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_req_header [in]: TLP request header
 */
static inline void handle_tlp_req_memory_read(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct pci_device_config *dev_cfg,
					      struct tlp_context *tlp_ctx,
					      const void *tlp_req_header)
{
	uint16_t length = GET_TLP_REQ_LENGTH(tlp_req_header);
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint64_t address;
	void *cpl_header = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);
	void *cpl_data = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_data(tlp_req);
	uint32_t *data_array = (uint32_t *)cpl_data;
	unsigned num_dwords = (length == 0) ? 1024 : length;

	/* Cap to buffer size to prevent out-of-bounds writes and malformed completions.
	 * TLP_DATA_ARRAY_MAX_SIZE (256 DWORDs = 1KB) is the maximum we can return. */
	if (num_dwords > TLP_DATA_ARRAY_MAX_SIZE) {
		DOCA_LOG_WARN("Memory read request for %u DWORDs exceeds max %u, capping",
			      num_dwords,
			      TLP_DATA_ARRAY_MAX_SIZE);
		num_dwords = TLP_DATA_ARRAY_MAX_SIZE;
	}

	DOCA_LOG_DBG("Handle TLP Request for Memory Read");

	/* Parse address based on format */
	if (fmt == TLP_FMT_4DW_NODATA) {
		uint32_t addr_high = GET_MEM_ADDR_64BIT_HIGH(tlp_req_header);
		uint32_t addr_low = GET_MEM_ADDR_64BIT_LOW(tlp_req_header);
		address = ((uint64_t)addr_high << 32) | (addr_low << 2);
	} else {
		address = GET_MEM_ADDR_32BIT(tlp_req_header);
	}

	uint16_t first_dw_be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	uint16_t last_dw_be = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);

	DOCA_LOG_DBG("Memory Read: addr=0x%lx, len=%d DWs, first_be=0x%x, last_be=0x%x",
		     address,
		     num_dwords,
		     first_dw_be,
		     last_dw_be);

	/* Get BAR base address */
	uint64_t bar0_base = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
			     (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
	uint64_t offset_in_bar = address - bar0_base;

	/* Transaction region starts at offset 0x3000 within BAR0 */
	const uint64_t transaction_region_start = TRANSACTION_REGION_START;
	const uint64_t transaction_region_end = transaction_region_start + tlp_ctx->transaction_region_size;

	/* Get PF index for this device to access its independent transaction region */
	uint32_t pf_index;
	doca_error_t ret = get_pf_index_for_device(tlp_ctx, dev_cfg, &pf_index);

	/* Read from transaction region if available and address in range.
	 * Must validate that entire read fits within region to prevent buffer overread. */
	if (ret == DOCA_SUCCESS && tlp_ctx->transaction_region_memories != NULL &&
	    tlp_ctx->transaction_region_memories[pf_index] != NULL && offset_in_bar >= transaction_region_start &&
	    offset_in_bar < transaction_region_end) {
		uint64_t offset_in_region = offset_in_bar - transaction_region_start;
		uint64_t read_end = offset_in_region + (uint64_t)num_dwords * BYTES_IN_DWORD;
		unsigned valid_dwords = num_dwords;

		/* Check if read would exceed region bounds */
		if (read_end > tlp_ctx->transaction_region_size) {
			DOCA_LOG_WARN("Memory read would exceed region bounds: offset=0x%lx, size=%u, region_size=%zu",
				      offset_in_region,
				      num_dwords * BYTES_IN_DWORD,
				      tlp_ctx->transaction_region_size);
			/* Truncate to fit within region, fill rest with dummy data */
			if (offset_in_region >= tlp_ctx->transaction_region_size) {
				valid_dwords = 0;
			} else {
				valid_dwords = (tlp_ctx->transaction_region_size - offset_in_region) / BYTES_IN_DWORD;
			}
		}

		uint8_t *region_ptr = (uint8_t *)tlp_ctx->transaction_region_memories[pf_index] + offset_in_region;
		for (unsigned dw_idx = 0; dw_idx < valid_dwords; dw_idx++) {
			uint8_t *dword_ptr = region_ptr + (dw_idx * 4);
			uint32_t dword_data = 0;
			uint8_t *data_ptr = (uint8_t *)&dword_data;
			unsigned be_mask = get_dword_byte_enable_mask(dw_idx, num_dwords, first_dw_be, last_dw_be);
			for (unsigned byte_idx = 0; byte_idx < 4; byte_idx++) {
				if (be_mask & (1 << byte_idx)) {
					data_ptr[byte_idx] = dword_ptr[byte_idx];
				}
			}
			data_array[dw_idx] = dword_data;
			DOCA_LOG_DBG("  DW[%d]: data=0x%08x, be_mask=0x%x", dw_idx, dword_data, be_mask);
		}
		/* Fill remaining DWORDs with dummy data if read was truncated */
		for (unsigned dw_idx = valid_dwords; dw_idx < num_dwords; dw_idx++) {
			data_array[dw_idx] = DUMMY_READ_DATA_BASE + dw_idx;
		}
		DOCA_LOG_DBG("  Read from transaction region PF%d at offset 0x%lx (in BAR: 0x%lx)",
			     pf_index,
			     offset_in_region,
			     offset_in_bar);
	} else {
		for (unsigned i = 0; i < num_dwords; i++) {
			data_array[i] = DUMMY_READ_DATA_BASE + i;
		}
		DOCA_LOG_DBG("  Using dummy data (offset 0x%lx outside transaction region 0x%lx-0x%lx)",
			     offset_in_bar,
			     transaction_region_start,
			     transaction_region_end - 1);
	}

	/* Set completion header - pass parameters directly to avoid race conditions.
	 * CRITICAL: For memory TLPs, DW2 contains address, NOT BDF. We MUST pass the
	 * device's actual BDF as completer_id_override to avoid malformed completions
	 * that cause Unexpected Completion AER errors on the host. */
	set_tlp_req_completion_header(cpl_header,
				      tlp_req,
				      TLP_FMT_CPL_W_DATA,
				      TLP_CPL_STATUS_SC,
				      num_dwords,
				      num_dwords * BYTES_IN_DWORD,
				      dev_cfg->bdf);
	DOCA_LOG_DBG("Memory Read: addr=0x%lx, len=%d DWs, cmpl_len=%d", address, length, num_dwords);
}

/*
 * Handle TLP memory write request (MMIO Write)
 *
 * @tlp_req [in]: TLP request
 * @dev_cfg [in]: Target device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_req_header [in]: TLP request header
 */
static inline void handle_tlp_req_memory_write(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					       struct pci_device_config *dev_cfg,
					       struct tlp_context *tlp_ctx,
					       const void *tlp_req_header)
{
	unsigned length = GET_TLP_REQ_LENGTH(tlp_req_header);
	unsigned first_dw_be = GET_TLP_REQ_FIRST_DW_BE(tlp_req_header);
	unsigned last_dw_be = GET_TLP_REQ_LAST_DW_BE(tlp_req_header);
	uint8_t fmt = GET_TLP_REQ_FMT(tlp_req_header);
	uint64_t address;
	unsigned num_dwords = (length == 0) ? 1024 : length;

	DOCA_LOG_DBG("Handle TLP Request for Memory Write");

	/* Parse address based on format */
	if (fmt == TLP_FMT_4DW_W_DATA) {
		uint32_t addr_high = GET_MEM_ADDR_64BIT_HIGH(tlp_req_header);
		uint32_t addr_low = GET_MEM_ADDR_64BIT_LOW(tlp_req_header);
		address = ((uint64_t)addr_high << 32) | (addr_low << 2);
	} else {
		address = GET_MEM_ADDR_32BIT(tlp_req_header);
	}

	const void *req_data = doca_devemu_pci_tlp_channel_req_get_tlp_data(tlp_req);

	DOCA_LOG_DBG("Memory Write: addr=0x%lx, len=%d DWs, first_be=0x%x, last_be=0x%x",
		     address,
		     num_dwords,
		     first_dw_be,
		     last_dw_be);

	/* Get BAR base address */
	uint64_t bar0_base = ((uint64_t)dev_cfg->cfg_space_hdr.type0.bar[1] << 32) |
			     (dev_cfg->cfg_space_hdr.type0.bar[0] & 0xFFFFFFF0);
	uint64_t offset_in_bar = address - bar0_base;

	/* Transaction region starts at offset 0x3000 within BAR0 */
	const uint64_t transaction_region_start = TRANSACTION_REGION_START;
	const uint64_t transaction_region_end = transaction_region_start + tlp_ctx->transaction_region_size;

	/* Get PF index for this device to access its independent transaction region */
	uint32_t pf_index;
	doca_error_t ret = get_pf_index_for_device(tlp_ctx, dev_cfg, &pf_index);

	/* Write to transaction region if available and address in range.
	 * Must validate that entire write fits within region to prevent buffer overflow. */
	if (ret == DOCA_SUCCESS && req_data != NULL && tlp_ctx->transaction_region_memories != NULL &&
	    tlp_ctx->transaction_region_memories[pf_index] != NULL && offset_in_bar >= transaction_region_start &&
	    offset_in_bar < transaction_region_end) {
		uint64_t offset_in_region = offset_in_bar - transaction_region_start;
		uint64_t write_end = offset_in_region + (uint64_t)num_dwords * BYTES_IN_DWORD;

		/* Check if write would exceed region bounds */
		if (write_end > tlp_ctx->transaction_region_size) {
			DOCA_LOG_WARN("Memory write would exceed region bounds: offset=0x%lx, size=%u, region_size=%zu",
				      offset_in_region,
				      num_dwords * BYTES_IN_DWORD,
				      tlp_ctx->transaction_region_size);
			/* Truncate to fit within region */
			if (offset_in_region >= tlp_ctx->transaction_region_size) {
				num_dwords = 0; /* Completely out of bounds */
			} else {
				num_dwords = (tlp_ctx->transaction_region_size - offset_in_region) / BYTES_IN_DWORD;
			}
		}

		uint8_t *region_ptr = (uint8_t *)tlp_ctx->transaction_region_memories[pf_index] + offset_in_region;
		const uint32_t *data_array = (const uint32_t *)req_data;
		for (unsigned dw_idx = 0; dw_idx < num_dwords; dw_idx++) {
			uint32_t dword_data = data_array[dw_idx];
			uint8_t *dword_ptr = region_ptr + (dw_idx * 4);
			uint8_t *data_ptr = (uint8_t *)&dword_data;
			unsigned be_mask = get_dword_byte_enable_mask(dw_idx, num_dwords, first_dw_be, last_dw_be);
			for (unsigned byte_idx = 0; byte_idx < 4; byte_idx++) {
				if (be_mask & (1 << byte_idx)) {
					dword_ptr[byte_idx] = data_ptr[byte_idx];
				}
			}
			DOCA_LOG_DBG("  DW[%d]: data=0x%08x, be_mask=0x%x", dw_idx, dword_data, be_mask);
		}
		DOCA_LOG_DBG("  Wrote to transaction region PF%d at offset 0x%lx (in BAR: 0x%lx)",
			     pf_index,
			     offset_in_region,
			     offset_in_bar);
	} else {
		DOCA_LOG_DBG("  Write ignored (offset 0x%lx outside transaction region 0x%lx-0x%lx)",
			     offset_in_bar,
			     transaction_region_start,
			     transaction_region_end - 1);
	}

	/* Memory write is posted - set completion context to 0 */
	dev_cfg->cmpl_fmt = 0;
	dev_cfg->cmpl_length = 0;
	dev_cfg->cmpl_status = 0;
	DOCA_LOG_DBG("Memory Write: addr=0x%lx, len=%d DWs", address, length);
}

/*
 * Return Unsupported Request (UR) completion for invalid requests
 *
 * @tlp_req [in]: TLP request
 * @dev_cfg [in]: Device configuration (used for completer_id - can be dummy device)
 * @tlp_type [in]: TLP request type (to determine if completer_id should be extracted from header)
 *
 * For config TLPs, the BDF is in DW2 of the header and can be extracted.
 * For memory TLPs, DW2 contains the address - we must use dev_cfg->bdf for completer_id.
 */
static inline void return_unsupported_request(struct doca_devemu_pci_tlp_channel_req *tlp_req,
					      struct pci_device_config *dev_cfg,
					      enum tlp_req_type tlp_type)
{
	void *cpl_header = doca_devemu_pci_tlp_channel_req_get_tlp_cpl_header(tlp_req);

	/* For config TLPs, pass 0 to extract BDF from header (DW2 contains BDF).
	 * For memory TLPs, pass dev_cfg->bdf as DW2 contains address, not BDF.
	 * This prevents malformed completions that cause Unexpected Completion AER errors. */
	uint16_t completer_id_override = 0;
	if (tlp_type == TLP_REQ_TYPE_MEMORY_READ || tlp_type == TLP_REQ_TYPE_MEMORY_WRITE) {
		completer_id_override = dev_cfg->bdf;
	}

	set_tlp_req_completion_header(cpl_header,
				      tlp_req,
				      TLP_FMT_CPL_NODATA,
				      TLP_CPL_STATUS_UR,
				      0,
				      BYTES_IN_DWORD,
				      completer_id_override);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, 1, NULL);
}

/*
 * Handle TLP request for bridge topology (unified entry point)
 * Follows tlp_handler sample style with clear structure
 *
 * @tlp_req [in]: TLP request
 * @dev_cfg [in]: Target device configuration
 * @tlp_ctx [in]: TLP context
 * @tlp_type [in]: TLP request type
 */
static inline void handle_tlp_req(struct doca_devemu_pci_tlp_channel_req *tlp_req,
				  struct pci_device_config *dev_cfg,
				  struct tlp_context *tlp_ctx,
				  enum tlp_req_type tlp_type)
{
	const void *req_header = doca_devemu_pci_tlp_channel_req_get_tlp_header(tlp_req);
	uint32_t ext_reg_num = GET_TLP_REQ_EXT_REG_NUM(req_header);
	uint16_t cap_id = 0;
	bool is_cap_id_valid = false;
	bool is_pcie_cap = false;

	/* NOTE: ALL completion fields (requester_id, tag, completer_id, cmpl_fmt, cmpl_status,
	 * cmpl_length) are now passed directly to set_tlp_req_completion_header() as parameters
	 * or extracted from tlp_req inside that function. This prevents race conditions with
	 * concurrent TLP requests from different devices - no shared state is used. */

	/* Handle Dummy device - return UR */
	if (dev_cfg->is_dummy) {
		return_unsupported_request(tlp_req, dev_cfg, tlp_type);
		return;
	}

	switch (tlp_type) {
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_0: {
		handle_tlp_req_read_type_0(tlp_req,
					   dev_cfg,
					   tlp_ctx,
					   ext_reg_num,
					   &cap_id,
					   &is_cap_id_valid,
					   &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_CONFIG_READ_TYPE_1: {
		handle_tlp_req_read_type_1(tlp_req,
					   dev_cfg,
					   tlp_ctx,
					   ext_reg_num,
					   &cap_id,
					   &is_cap_id_valid,
					   &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_0: {
		handle_tlp_req_write_type_0(tlp_req,
					    dev_cfg,
					    tlp_ctx,
					    ext_reg_num,
					    req_header,
					    &cap_id,
					    &is_cap_id_valid,
					    &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_CONFIG_WRITE_TYPE_1: {
		handle_tlp_req_write_type_1(tlp_req,
					    dev_cfg,
					    tlp_ctx,
					    ext_reg_num,
					    req_header,
					    &cap_id,
					    &is_cap_id_valid,
					    &is_pcie_cap);
		return;
	}
	case TLP_REQ_TYPE_MEMORY_READ: {
		if (dev_cfg->is_endpoint) {
			vnet_pci_dev_handle_mmio_read(tlp_req, tlp_ctx, dev_cfg);
			return;
		}
		handle_tlp_req_memory_read(tlp_req, dev_cfg, tlp_ctx, req_header);
		break;
	}
	case TLP_REQ_TYPE_MEMORY_WRITE: {
		handle_tlp_req_memory_write(tlp_req, dev_cfg, tlp_ctx, req_header);
		if (dev_cfg->is_endpoint) {
			vnet_pci_dev_handle_mmio_write(tlp_req, tlp_ctx, dev_cfg);
			return;
		}
		break;
	}
	default:
		DOCA_LOG_ERR("Unsupported TLP request type: %d", tlp_type);
		return_unsupported_request(tlp_req, dev_cfg, tlp_type);
		return;
	}

	/* Complete memory requests (non-config).
	 * Guard against NULL tlp_dev which can occur if:
	 * 1. Device is a bridge (bridges don't have tlp_dev)
	 * 2. Endpoint is being destroyed (race with hotplug removal)
	 * The API accepts NULL for tlp_dev in these cases. */
	uint8_t is_non_posted = (uint8_t)(tlp_type == TLP_REQ_TYPE_MEMORY_READ);
	doca_devemu_pci_tlp_channel_req_complete_tlp(tlp_req, is_non_posted, dev_cfg->tlp_dev);
}

/**
 * @brief TLP Event Callback - Main Request Dispatcher
 *
 * Central TLP request handler that receives all PCIe transactions from the host
 * and routes them to appropriate specialized handlers based on request type.
 * Supports configuration space, memory-mapped I/O, and other PCIe transaction types.
 *
 * @param[in] channel TLP channel context (unused)
 * @param[in] tlp_req TLP request from host to process
 * @param[in] req_user_data User data associated with request (unused)
 *
 * @note This is the main entry point for all PCIe transactions
 * @note Unsupported request types generate UR (Unsupported Request) completions
 */
static void vnet_pci_dev_event_cb(struct doca_devemu_pci_tlp_channel *channel,
				  struct doca_devemu_pci_tlp_channel_req *tlp_req,
				  void *req_user_data)
{
	(void)req_user_data; /* unused */

	struct tlp_context *tlp_ctx;
	union doca_data channel_user_data;
	doca_error_t result;

	result = doca_ctx_get_user_data(doca_devemu_pci_tlp_channel_as_ctx(channel), &channel_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from tlp channel context: %s", doca_error_get_descr(result));
		return;
	}
	tlp_ctx = (struct tlp_context *)channel_user_data.ptr;

	/* Check for ACG (Asynchronous Credit Grant) opcode */
	enum doca_devemu_pci_tlp_channel_req_opcode opcode = doca_devemu_pci_tlp_channel_req_get_opcode(tlp_req);
	if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_ACG) {
		DOCA_LOG_DBG("Received ACG credit from FW");
		doca_error_t ret = acg_queue_push(tlp_ctx, tlp_req);
		if (ret == DOCA_ERROR_NO_MEMORY) {
			if (tlp_ctx->acg_queue == NULL) {
				DOCA_LOG_DBG("ACG queue unavailable (cleanup in progress), discarding credit");
			} else {
				uint16_t count, size;

				acg_queue_stats_get(tlp_ctx, &count, &size);
				DOCA_LOG_DBG("ACG queue full (%u/%u), discarding new credit", count, size);
			}
			doca_devemu_pci_tlp_channel_req_complete_acg(
				tlp_req,
				0,
				DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_ACG_COMP_OPMODE_FLUSH);
		} else {
			uint16_t count, size;

			acg_queue_stats_get(tlp_ctx, &count, &size);
			DOCA_LOG_DBG("ACG credit queued (%u/%u)", count, size);
		}
		return;
	}

	/* Handle PCI_EVENT opcode (PERST assert/deassert) */
	if (opcode == DOCA_DEVEMU_PCI_TLP_CHANNEL_REQ_OPCODE_PCI_EVENT) {
		DOCA_LOG_DBG("Received PCI_EVENT request");
		/* Complete the PCI_EVENT request directly */
		doca_devemu_pci_tlp_channel_req_complete_pci_event(tlp_req);
		return;
	}

	enum tlp_req_type tlp_type = get_tlp_req_type(tlp_req);

	/* Find target device based on TLP type (memory vs config).
	 * For memory requests to endpoints, the read lock is acquired and must be released
	 * after completing the TLP request to prevent races with hotplug removal. */
	bool ep_lock_held = false;
	struct pci_device_config *dev_cfg = find_target_device(tlp_req, tlp_ctx, tlp_type, &ep_lock_held);

	handle_tlp_req(tlp_req, dev_cfg, tlp_ctx, tlp_type);

	/* Release endpoint read lock after TLP request is fully processed.
	 * This ensures tlp_dev remains valid throughout the entire request handling,
	 * preventing races with hotplug removal which takes the write lock. */
	if (ep_lock_held)
		pthread_rwlock_unlock(&dev_cfg->endpoint_lock);
}

/************************************************************************
 ******                 Device Lifecycle Management                ******
 ************************************************************************/

doca_error_t vnet_pci_device_create(struct tlp_context *tlp_ctx, const struct vnet_pci_device_attrs *attr)
{
	doca_error_t err;
	uint32_t i;

	if (tlp_ctx->num_ep == 0) {
		DOCA_LOG_INFO("No TLP devices to create (bridge-only topology)");
		return DOCA_SUCCESS;
	}

	/* Create and start TLP device for each endpoint */
	for (i = 0; i < tlp_ctx->num_ep; i++) {
		struct vnet_pci_device_attrs attr_i = *attr;
		struct vnet_virtio_net_config local_net_cfg;

		/* Create per-device config with unique MAC address.
		 * Use base MAC + device index to match offload engine MAC scheme.
		 * Copy to local struct to avoid mutating the shared base config. */
		if (attr->dev_cfg) {
			const struct vnet_virtio_net_config *base_cfg =
				(const struct vnet_virtio_net_config *)attr->dev_cfg;

			local_net_cfg = *base_cfg;

			/* Add device index to last octet with overflow handling */
			uint16_t new_octet = (uint16_t)local_net_cfg.mac[5] + i;
			if (new_octet > 255) {
				DOCA_LOG_ERR("MAC address overflow for device %d (base mac[5]=%u + index=%u > 255)",
					     i,
					     base_cfg->mac[5],
					     i);
				err = DOCA_ERROR_INVALID_VALUE;
				goto error_rollback;
			}
			local_net_cfg.mac[5] = (uint8_t)new_octet;

			attr_i.dev_cfg = &local_net_cfg;

			DOCA_LOG_INFO("Creating VirtIO device %d with MAC address: %02x:%02x:%02x:%02x:%02x:%02x",
				      i,
				      local_net_cfg.mac[0],
				      local_net_cfg.mac[1],
				      local_net_cfg.mac[2],
				      local_net_cfg.mac[3],
				      local_net_cfg.mac[4],
				      local_net_cfg.mac[5]);
		} else {
			DOCA_LOG_INFO("Creating VirtIO device %d with default config", i);
		}

		err = vnet_pci_device_init_internal(&tlp_ctx->virtio_dev[i], i, &attr_i);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize device %u: %s", i, doca_error_get_descr(err));
			goto error_rollback;
		}
	}

	DOCA_LOG_INFO("VNet device created and started successfully");
	DOCA_LOG_DBG("Device is now ready for host enumeration");
	return DOCA_SUCCESS;

error_rollback:
	/* Roll back already-initialized devices to prevent leaks */
	DOCA_LOG_INFO("Rolling back %u already-initialized device(s)", i);
	for (uint32_t j = 0; j < i; j++) {
		struct vnet_pci_device *dev = &tlp_ctx->virtio_dev[j];

		/* Free VQ array if allocated */
		if (dev->vqs) {
			free(dev->vqs);
			dev->vqs = NULL;
		}
		dev->vqs_count = 0;
		DOCA_LOG_DBG("Rolled back device %u", j);
	}
	return err; /* Preserve original error code for diagnosability */
}

/**
 * @brief Initialize VirtIO device
 *
 * Initializes the VirtIO device with the specified attributes using a modular
 * template creation approach. Creates the base template using helper functions,
 * then customizes it based on device type, queue count, and other attributes.
 *
 * @param[in] dev Device to initialize
 * @param[in] attr Device attributes including device type, queue count, callbacks
 * @return DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t vnet_pci_device_init_internal(struct vnet_pci_device *dev,
						  uint32_t index,
						  const struct vnet_pci_device_attrs *attr)
{
	/* Set PF index for this device */
	dev->pf_index = index;

	/* Create VirtIO template using modular helper functions */
	dev->pcie_dev = vnet_pci_device_create_virtio_template();
	if (attr->virtio_type != VNET_VIRTIO_INVALID_DEVICE) {
		/* Update PCI device ID and subsystem ID based on device type */
		dev->pcie_dev.cfg.regs.device_id += attr->virtio_type;
		dev->pcie_dev.cfg.regs.subsystem_id = attr->virtio_type;

		/* Update PCI class code based on device type */
		if (attr->virtio_type == VNET_VIRTIO_NETWORK_DEVICE) {
			dev->pcie_dev.cfg.regs.class_code = PCI_CLASS_NETWORK;
			dev->pcie_dev.cfg.regs.subclass = 0x00; /* Ethernet controller */
		}
	}

	/* Initialize VirtIO structures with proper defaults */
	/* Initialize PCI Common Configuration */
	struct vnet_virtio_common_config *pci_cfg = &dev->pci_cfg;
	memset(pci_cfg, 0, sizeof(*pci_cfg));
	pci_cfg->num_queues = attr->num_queues;
	pci_cfg->config_generation = 1;
	pci_cfg->config_msix_vector = VIRTIO_MSI_NO_VECTOR;

	/* Initialize device features */
	dev->device_features = attr->device_features | (1ULL << VNET_VIRTIO_F_VERSION_1) |
			       (1ULL << VNET_VIRTIO_F_ACCESS_PLATFORM) | (1ULL << VIRTIO_NET_F_CTRL_VQ) |
			       (1ULL << VIRTIO_NET_F_MQ);

	/* Initialize PCI config registers */
	pci_cfg->device_feature = dev->device_features & 0xFFFFFFFF; /* Lower 32 bits initially */
	pci_cfg->device_status = VNET_VIRTIO_DEVICE_STATUS_NEEDS_RESET;
	if (attr->num_queues > 0)
		pci_cfg->num_queues = attr->num_queues;

	/* Initialize network device configuration */
	struct vnet_virtio_net_config *net_cfg = &dev->vnet_cfg;
	memset(net_cfg, 0, sizeof(*net_cfg));

	if (attr->dev_cfg) {
		/* Use provided device configuration */
		memcpy(net_cfg, attr->dev_cfg, sizeof(*net_cfg));
		DOCA_LOG_INFO("Using provided device configuration");
	} else {
		/* Use default configuration */
		memcpy(net_cfg->mac, "\x52\x54\x00\x12\x34\x56", ETH_ALEN); /* Default MAC */
		net_cfg->status = VIRTIO_NET_S_LINK_UP;			    /* Link up */
		net_cfg->max_virtqueue_pairs = VNET_MAX_QUEUE_PAIRS;
		net_cfg->mtu = 1500;
		net_cfg->speed = 1000; /* 1 Gbps */
		net_cfg->duplex = 1;   /* Full duplex */
		net_cfg->rss_max_key_size = 40;
		net_cfg->rss_max_indirection_table_length = 128;
		net_cfg->supported_hash_types = 0x3F; /* Support common hash types */
		DOCA_LOG_INFO("Using default device configuration");
	}

	/* Allocate VQ shadow array dynamically based on actual num_queues */
	dev->vqs_count = attr->num_queues;
	dev->queue_size = attr->queue_size ? attr->queue_size : VNET_DEFAULT_QUEUE_SIZE; /* Set default if not specified
											  */
	dev->vqs = calloc(dev->vqs_count, sizeof(struct vnet_virtio_queue_config));
	if (!dev->vqs) {
		DOCA_LOG_ERR("Failed to allocate VQ shadow array for %d queues", dev->vqs_count);
		return DOCA_ERROR_NO_MEMORY;
	}
	DOCA_LOG_DBG("Allocated VQ shadow array for %d queues (%zu bytes), queue_size=%u",
		     dev->vqs_count,
		     dev->vqs_count * sizeof(struct vnet_virtio_queue_config),
		     dev->queue_size);

	/* Initialize VirtQueue configurations */
	vnet_pci_device_queue_init(dev);

	/* Store callback and callback arguments from attributes */
	dev->pci_cfg_change_cb = attr->pci_cfg_change_cb;
	dev->cb_arg = attr->cb_arg;

	/* Initialize per-device status tracking (0xFF = uninitialized) */
	dev->prev_status = 0xFF;
	atomic_init(&dev->reset_generation, 1);
	atomic_init(&dev->cancel_in_progress, false);

	DOCA_LOG_DBG("VNet device initialized with:");
	DOCA_LOG_DBG("  - num_queues=%d, features=0x%lx", attr->num_queues, dev->device_features);
	DOCA_LOG_DBG("  - device_feature register=0x%08x (here VIRTIO_F_VERSION_1=%s)",
		     pci_cfg->device_feature, /* VIRTIO_F_VERSION_1 will be visible when select=1 */
		     (dev->device_features & (1ULL << VNET_VIRTIO_F_VERSION_1)) ? "YES" : "NO");

	return DOCA_SUCCESS;
}

void vnet_pci_device_destroy(struct vnet_pci_device *dev)
{
	if (!dev)
		return;

	/* Free dynamically allocated VQ shadow array */
	if (dev->vqs) {
		free(dev->vqs);
		dev->vqs = NULL;
		dev->vqs_count = 0;
	}

	DOCA_LOG_INFO("VirtIO device destroyed");
}

/**
 * @brief Initialize VNet PCI Device Framework
 *
 * Sets up the global framework state and initializes device management structures.
 * Creates and configures the PCI TLP device type, sets up device lookup arrays,
 * and prepares the framework for device creation and enumeration.
 *
 * @param[in] dev DOCA device handle for the DPU hardware
 * @return DOCA_SUCCESS on success, DOCA_ERROR otherwise
 *
 * @note This must be called before any device operations
 * @note Automatically calls vnet_pci_device_init() for TLP setup
 * @note Framework must be cleaned up with vnet_pci_dev_reset()
 */
doca_error_t vnet_pci_dev_init(struct tlp_context *tlp_ctx)
{
	doca_error_t result;

	LIST_INIT(&vnet_pci_dev_enumerated_devs);
	memset(vnet_pci_dev_space, 0, sizeof(vnet_pci_dev_space));

	/* Initialize PCI config workqueue */
	result = pci_cfg_workqueue_init();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to initialize PCI config workqueue: %s", doca_error_get_name(result));
		return result;
	}

	result = vnet_pci_device_init(tlp_ctx);
	if (result != DOCA_SUCCESS) {
		pci_cfg_workqueue_destroy();
		return result;
	}

	return DOCA_SUCCESS;
}

/**
 * @brief Query and validate predefined VIRTIO_NET bar layout
 *
 * This function queries the predefined VIRTIO_NET bar layout that was automatically
 * set by doca_devemu_vnet_pci_tlp_type_create() and validates the firmware-provided
 * layout configuration.
 *
 * VNet devices use the MLX5_GENERIC_EMU_DEV_TYPE_OBJ_BAR_LAYOUT_VIRTIO_NET predefined layout,
 * so we only need to query and validate the firmware-provided configuration.
 *
 * @param[in] vnet_pci_type VNet PCI TLP type (must be started)
 * @return DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
static doca_error_t vnet_pci_device_query_predefined_layout(struct doca_devemu_pci_type *vnet_pci_type)
{
	doca_error_t err;
	uint32_t i, n;

	if (!vnet_pci_type) {
		DOCA_LOG_ERR("VNet PCI type is NULL");
		return DOCA_ERROR_INVALID_VALUE;
	}

	DOCA_LOG_INFO("Querying predefined VIRTIO_NET bar layout from firmware");

	/* ========================================================================
	 * Step 1: Query and validate BAR Information
	 * ======================================================================== */
	struct doca_devemu_pci_bar_info **bar_list;
	err = doca_devemu_pci_type_create_bar_info_list(vnet_pci_type, &bar_list, &n);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VNet bar info list: %s", doca_error_get_descr(err));
		return err;
	}

	DOCA_LOG_INFO("Found %u BARs in predefined VIRTIO_NET layout", n);

	for (i = 0; i < n; i++) {
		uint8_t id, log_sz, is_prefetch;
		enum doca_devemu_pci_bar_mem_type mem_type;

		doca_devemu_pci_bar_info_get_bar_id(bar_list[i], &id);
		doca_devemu_pci_bar_info_get_log_sz(bar_list[i], &log_sz);
		doca_devemu_pci_bar_info_get_mem_type(bar_list[i], &mem_type);
		doca_devemu_pci_bar_info_get_prefetchable(bar_list[i], &is_prefetch);

		DOCA_LOG_INFO("- BAR%u: log_size=%2u, mem_type=%d, prefetchable=%s",
			      id,
			      log_sz,
			      mem_type,
			      is_prefetch ? "yes" : "no");

		/* Validate expected VirtIO BAR configuration */
		if (id == VNET_VIRTIO_BAR_ID && mem_type != DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT) {
			DOCA_LOG_ERR("BAR%u must be 64-bit memory type for VirtIO", id);
			doca_devemu_pci_type_destroy_bar_info_list(bar_list);
			return DOCA_ERROR_INVALID_VALUE;
		}
	}

	doca_devemu_pci_type_destroy_bar_info_list(bar_list);

	/* ========================================================================
	 * Step 2: Query and validate Transaction (MMIO) Regions
	 * ======================================================================== */
	struct doca_devemu_pci_transaction_region_info **mmio_list;
	err = doca_devemu_pci_type_create_transaction_region_info_list(vnet_pci_type, &mmio_list, &n);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VNet MMIO region list: %s", doca_error_get_descr(err));
		return err;
	}

	if (n == 0) {
		DOCA_LOG_ERR("VNet type must have at least one MMIO transaction region");
		doca_devemu_pci_type_destroy_transaction_region_info_list(mmio_list);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (n > 1) {
		DOCA_LOG_WARN("Expected one MMIO region, found %u. Using first region", n);
	}

	for (i = 0; i < n; i++) {
		uint8_t bar_id;
		uint64_t start_addr, size;

		doca_devemu_pci_transaction_region_info_get_bar_id(mmio_list[i], &bar_id);
		doca_devemu_pci_transaction_region_info_get_start_addr(mmio_list[i], &start_addr);
		doca_devemu_pci_transaction_region_info_get_size(mmio_list[i], &size);

		DOCA_LOG_INFO("MMIO region %u:\t BAR%u, start=0x%lx, size=0x%lx", i, bar_id, start_addr, size);

		/* Use first region for VirtIO capabilities mapping */
		if (i == 0) {
			/* Validate region size covers our VirtIO device configuration */
			const uint64_t min_required_size = VNET_VIRTIO_DEV_CFG_OFFSET + VNET_VIRTIO_DEV_CFG_LEN;
			if (size < min_required_size) {
				DOCA_LOG_ERR("MMIO region too small: 0x%lx, required: 0x%lx", size, min_required_size);
				doca_devemu_pci_type_destroy_transaction_region_info_list(mmio_list);
				return DOCA_ERROR_INVALID_VALUE;
			}

			DOCA_LOG_DBG("VirtIO capabilities will be mapped to BAR%u starting at 0x%lx",
				     bar_id,
				     start_addr);
		}
	}

	doca_devemu_pci_type_destroy_transaction_region_info_list(mmio_list);

	/* ========================================================================
	 * Step 3: Query and validate Doorbell Regions
	 * ======================================================================== */
	struct doca_devemu_pci_db_region_by_data_info **db_list;
	err = doca_devemu_pci_type_create_db_region_by_data_info_list(vnet_pci_type, &db_list, &n);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VNet doorbell region list: %s", doca_error_get_descr(err));
		return err;
	}

	if (n == 0) {
		DOCA_LOG_ERR("VNet type must have at least one doorbell region");
		doca_devemu_pci_type_destroy_db_region_by_data_info_list(db_list);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (n > 1) {
		DOCA_LOG_WARN("Expected one doorbell region, found %u. Using first region", n);
	}

	for (i = 0; i < n; i++) {
		uint8_t bar_id;
		uint64_t start_addr, size;

		doca_devemu_pci_db_region_by_data_info_get_bar_id(db_list[i], &bar_id);
		doca_devemu_pci_db_region_by_data_info_get_start_addr(db_list[i], &start_addr);
		doca_devemu_pci_db_region_by_data_info_get_size(db_list[i], &size);

		DOCA_LOG_INFO("Doorbell region %u:\t BAR%u, start=0x%lx, size=0x%lx", i, bar_id, start_addr, size);
	}

	doca_devemu_pci_type_destroy_db_region_by_data_info_list(db_list);

	/* ========================================================================
	 * Step 4: Query and validate MSI-X Table Region
	 * ======================================================================== */
	struct doca_devemu_pci_msix_table_region_info **msix_list;
	err = doca_devemu_pci_type_create_msix_table_region_info_list(vnet_pci_type, &msix_list, &n);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VNet MSI-X table region list: %s", doca_error_get_descr(err));
		return err;
	}

	if (n == 0) {
		DOCA_LOG_ERR("VNet type must have at least one MSI-X table region");
		doca_devemu_pci_type_destroy_msix_table_region_info_list(msix_list);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (n > 1) {
		DOCA_LOG_WARN("Expected one MSI-X table region, found %u. Using first region", n);
	}

	for (i = 0; i < n; i++) {
		uint8_t bar_id;
		uint64_t start_addr, size;

		doca_devemu_pci_msix_table_region_info_get_bar_id(msix_list[i], &bar_id);
		doca_devemu_pci_msix_table_region_info_get_start_addr(msix_list[i], &start_addr);
		doca_devemu_pci_msix_table_region_info_get_size(msix_list[i], &size);

		DOCA_LOG_INFO("MSI-X table region %u:\t BAR%u, start=0x%lx, size=0x%lx", i, bar_id, start_addr, size);
	}

	doca_devemu_pci_type_destroy_msix_table_region_info_list(msix_list);

	/* ========================================================================
	 * Step 5: Query and validate MSI-X PBA Region
	 * ======================================================================== */
	struct doca_devemu_pci_msix_pba_region_info **pba_list;
	err = doca_devemu_pci_type_create_msix_pba_region_info_list(vnet_pci_type, &pba_list, &n);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create VNet MSI-X PBA region list: %s", doca_error_get_descr(err));
		return err;
	}

	if (n == 0) {
		DOCA_LOG_ERR("VNet type must have at least one MSI-X PBA region");
		doca_devemu_pci_type_destroy_msix_pba_region_info_list(pba_list);
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (n > 1) {
		DOCA_LOG_WARN("Expected one MSI-X PBA region, found %u. Using first region", n);
	}

	for (i = 0; i < n; i++) {
		uint8_t bar_id;
		uint64_t start_addr, size;

		doca_devemu_pci_msix_pba_region_info_get_bar_id(pba_list[i], &bar_id);
		doca_devemu_pci_msix_pba_region_info_get_start_addr(pba_list[i], &start_addr);
		doca_devemu_pci_msix_pba_region_info_get_size(pba_list[i], &size);

		DOCA_LOG_INFO("MSI-X PBA region %u:\t BAR%u, start=0x%lx, size=0x%lx", i, bar_id, start_addr, size);
	}

	doca_devemu_pci_type_destroy_msix_pba_region_info_list(pba_list);

	DOCA_LOG_INFO("Successfully validated predefined VIRTIO_NET bar layout");
	return DOCA_SUCCESS;
}

doca_error_t vnet_pci_device_init(struct tlp_context *tlp_ctx)
{
	doca_error_t err;
	struct doca_devinfo *devinfo;
	uint8_t supported;

	/* Get device info for capability check */
	devinfo = doca_dev_as_devinfo(tlp_ctx->dev);
	if (!devinfo) {
		DOCA_LOG_ERR("Failed to get device info");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check if VNet TLP type is supported by this device */
	err = doca_devemu_vnet_cap_is_pci_tlp_type_supported(devinfo, &supported);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to check VNet TLP type support: %s", doca_error_get_descr(err));
		return err;
	}
	if (supported != 1) {
		DOCA_LOG_ERR(
			"VNet TLP type is not supported by this device (virtio_net_bar_layout capability missing)");
		DOCA_LOG_ERR("This device does not have hardware support for VirtIO Network device emulation");
		return DOCA_ERROR_NOT_SUPPORTED;
	}
	DOCA_LOG_INFO("VNet TLP type is supported by this device - proceeding with creation");

	/* Create VNet TLP type */
	err = doca_devemu_vnet_pci_tlp_type_create("vnet_pci_dev", &tlp_ctx->pci_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create PCI TLP device type: %s", doca_error_get_descr(err));
		return err;
	}

	/* Set the device for this type */
	err = doca_devemu_pci_type_set_dev(tlp_ctx->pci_type, tlp_ctx->dev);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach device to PCI type: %s", doca_error_get_descr(err));
		goto error;
	}

	/* Set the number of MSI-X vectors to the number of queues */
	err = doca_devemu_pci_type_set_num_msix(tlp_ctx->pci_type, VNET_PCI_DEV_NUM_MSIX);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set number of MSI-X vectors: %s", doca_error_get_descr(err));
		goto error;
	}

	/* Set the number of doorbells supported by this device type */
	err = doca_devemu_pci_type_set_num_db(tlp_ctx->pci_type, VNET_PCI_DEV_NUM_DB);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set number of doorbells: %s", doca_error_get_descr(err));
		goto error;
	}

	/* Configure MSI-X capability */
	err = doca_devemu_pci_tlp_type_set_pci_cap_conf(tlp_ctx->pci_type,
							PCI_CAP_ID_MSIX,
							offsetof(struct pcie_virtio_dev, cfg.msix_cap),
							sizeof(struct msix_capability));
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set MSI-X capability configuration: %s", doca_error_get_descr(err));
		goto error;
	}

	/* Start the PCI type so we can create device representors */
	err = doca_devemu_pci_type_start(tlp_ctx->pci_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start PCI device type: %s", doca_error_get_descr(err));
		goto error;
	}

	DOCA_LOG_INFO("VNet PCI device type created successfully");

	/* Query predefined VIRTIO_NET bar layout after type is started */
	err = vnet_pci_device_query_predefined_layout(tlp_ctx->pci_type);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query predefined VIRTIO_NET bar layout: %s", doca_error_get_descr(err));
		goto error_started;
	}

	return DOCA_SUCCESS;

error_started:
	(void)doca_devemu_pci_type_stop(tlp_ctx->pci_type);

error:
	(void)doca_devemu_pci_type_destroy(tlp_ctx->pci_type);
	tlp_ctx->pci_type = NULL;
	return err;
}

/************************************************************************
 ******                    Framework Management                    ******
 ************************************************************************/

/*
 * Start vnet_pci_dev - create and configure TLP channel
 *
 * @return: DOCA_SUCCESS on success, DOCA_ERROR otherwise
 */
doca_error_t vnet_pci_dev_start(struct tlp_context *tlp_ctx)
{
	doca_error_t err;

	/* Create TLP channel */
	err = doca_devemu_pci_tlp_channel_create(tlp_ctx->dev, &tlp_ctx->tlp_channel);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create TLP channel: %s", doca_error_get_descr(err));
		return err;
	}

	/* Configure TLP request user data size */
	err = doca_devemu_pci_tlp_channel_set_req_user_data_size(tlp_ctx->tlp_channel, VNET_TLP_REQ_USER_DATA_SIZE);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set TLP request user data size: %s", doca_error_get_descr(err));
		doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		return err;
	}

	/* Register TLP request handler */
	err = doca_devemu_pci_tlp_channel_event_req_register(tlp_ctx->tlp_channel, vnet_pci_dev_event_cb);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register TLP request handler: %s", doca_error_get_descr(err));
		doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
		return err;
	}

	/* Enable ACG mechanism to receive credits for MMIO Write (MSI) - required for hotplug mode */
	if (tlp_ctx->hotplug_mode) {
		err = doca_devemu_pci_tlp_channel_set_acg_enabled(tlp_ctx->tlp_channel, 1);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_WARN("ACG not available: %s. Hotplug MSI disabled, host will poll",
				      doca_error_get_descr(err));
		} else {
			DOCA_LOG_INFO("ACG enabled for MSI interrupts");
		}
	}

	DOCA_LOG_INFO("VNet PCI device TLP channel created and configured");
	return DOCA_SUCCESS;
}

doca_error_t vnet_pci_dev_start_from_export(struct tlp_context *tlp_ctx,
					    const void *export_desc,
					    size_t export_desc_len,
					    const char *shm_dir_path)
{
	doca_error_t err;

	err = doca_devemu_pci_tlp_channel_create_from_export(export_desc,
							     export_desc_len,
							     shm_dir_path,
							     tlp_ctx->dev,
							     &tlp_ctx->tlp_channel);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create TLP channel from export: %s", doca_error_get_descr(err));
		return err;
	}

	err = doca_devemu_pci_tlp_channel_set_primary(tlp_ctx->tlp_channel, false);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set TLP channel as secondary: %s", doca_error_get_descr(err));
		goto cleanup;
	}

	err = doca_devemu_pci_tlp_channel_set_req_user_data_size(tlp_ctx->tlp_channel, VNET_TLP_REQ_USER_DATA_SIZE);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set TLP request user data size: %s", doca_error_get_descr(err));
		goto cleanup;
	}

	err = doca_devemu_pci_tlp_channel_event_req_register(tlp_ctx->tlp_channel, vnet_pci_dev_event_cb);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register TLP request handler: %s", doca_error_get_descr(err));
		goto cleanup;
	}

	if (tlp_ctx->hotplug_mode) {
		err = doca_devemu_pci_tlp_channel_set_acg_enabled(tlp_ctx->tlp_channel, true);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_WARN("ACG not available: %s. Hotplug MSI disabled, host will poll",
				      doca_error_get_descr(err));
		}
	}

	DOCA_LOG_INFO("VNet PCI device TLP channel created from export (secondary)");
	return DOCA_SUCCESS;

cleanup:
	doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
	tlp_ctx->tlp_channel = NULL;
	return err;
}

/*
 * Stop vnet_pci_dev
 */
void vnet_pci_dev_stop(struct tlp_context *tlp_ctx)
{
	if (!tlp_ctx->tlp_channel)
		return;

	doca_devemu_pci_tlp_channel_destroy(tlp_ctx->tlp_channel);
	tlp_ctx->tlp_channel = NULL;
}

/*
 * Get context for VNet PCI device TLP channel
 *
 * @return: DOCA context for the TLP channel
 */
struct doca_ctx *vnet_pci_dev_tlp_channel_ctx(struct tlp_context *tlp_ctx)
{
	return doca_devemu_pci_tlp_channel_as_ctx(tlp_ctx->tlp_channel);
}

/*
 * Reset vnet_pci_dev
 */
void vnet_pci_dev_reset(struct tlp_context *tlp_ctx)
{
	/* Cleanup any state */
	vnet_pci_dev_stop(tlp_ctx);

	/* Stop and destroy the PCI type (only if it exists and wasn't already stopped) */
	if (tlp_ctx->pci_type) {
		(void)doca_devemu_pci_type_stop(tlp_ctx->pci_type);
		(void)doca_devemu_pci_type_destroy(tlp_ctx->pci_type);
		tlp_ctx->pci_type = NULL;
	}

	/* Destroy PCI config workqueue */
	pci_cfg_workqueue_destroy();
}

/************************************************************************
 ******                  Device Accessor Functions                 ******
 ************************************************************************/

/*
 * Get PCI configuration for VirtIO device
 */
struct vnet_virtio_common_config *vnet_pci_device_get_pci_cfg(struct vnet_pci_device *dev)
{
	return &dev->pci_cfg;
}

/*
 * Get VirtIO queue configuration
 */
const struct vnet_virtio_queue_config *vnet_pci_device_get_virtq_pci_cfg(struct vnet_pci_device *dev)
{
	return dev->vqs;
}

/*
 * Get VirtIO network device configuration
 */
const struct vnet_virtio_net_config *vnet_pci_device_get_vnet_dev_cfg(struct vnet_pci_device *dev)
{
	return &dev->vnet_cfg;
}

/************************************************************************
******                 VirtIO Spec Compliance Functions           ******
************************************************************************/

void vnet_pci_device_vq_config(struct vnet_pci_device *dev, uint16_t vq_index)
{
	/* CVQ index calculated from device's configured max_queue_pairs (from -q flag) */
	uint16_t cvq_index = VNET_CVQ_INDEX(dev->vnet_cfg.max_virtqueue_pairs);
	struct vnet_pci_dev_controller *controller;
	struct vnet_pci_dev_resources *resources;
	struct vnet_virtio_common_config *common_cfg;
	struct vnet_pci_device *active_dev;
	struct doca_devemu_virtio_vq *vq_handle;
	doca_error_t result;
	uint16_t qp_idx;

	/* Skip if device reset is in progress - dev->vqs is being cleared.
	 * This prevents data race where we read dev->vqs while reset clears it. */
	if (pci_cfg_wq != NULL && pci_cfg_wq->reset_in_progress) {
		DOCA_LOG_DBG("VQ%d config skipped - device reset in progress", vq_index);
		return;
	}

	/* Get controller through callback arg with proper validation */
	resources = dev->cb_arg;
	if (!resources || !resources->tlp_ctx) {
		DOCA_LOG_DBG("No resources/tlp_ctx available - VQ config will be applied later");
		return;
	}
	if ((uint32_t)dev->pf_index >= resources->tlp_ctx->num_ep) {
		DOCA_LOG_ERR("Invalid pf_index %d >= num_ep %u", dev->pf_index, resources->tlp_ctx->num_ep);
		return;
	}
	controller = &resources->tlp_ctx->vnet_controller[dev->pf_index];
	active_dev = atomic_load(&controller->virtio_device);
	if (active_dev == NULL) {
		DOCA_LOG_DBG("Controller not initialized - VQ config will be applied later");
		return;
	}
	if (active_dev != dev) {
		DOCA_LOG_DBG("VQ%d config skipped - device is not active", vq_index);
		return;
	}

	/* Check if hardware is ready for VQ configuration */
	if (!atomic_load(&controller->vqs_initialized)) {
		DOCA_LOG_DBG("VQ structures not initialized yet - config will be applied later");
		return;
	}

	/* Check if VQ is fully configured by host - use vqs_count for bounds check */
	if (vq_index >= dev->vqs_count || !dev->vqs[vq_index].queue_enable || dev->vqs[vq_index].queue_desc == 0 ||
	    dev->vqs[vq_index].queue_driver == 0 || dev->vqs[vq_index].queue_device == 0) {
		DOCA_LOG_DBG("VQ%d not fully configured yet (vqs_count=%d)", vq_index, dev->vqs_count);
		return;
	}

	DOCA_LOG_DBG("VirtIO Spec Compliance: Applying VQ%d configuration", vq_index);

	/* Identify VQ type and get handle from arrays */
	if (vq_index == cvq_index && controller->mq_feature_negotiated) {
		/* CVQ - Only if MQ feature negotiated */
		if (!controller->cvq) {
			DOCA_LOG_ERR("CVQ not created but VQ%d configuration received", vq_index);
			return;
		}
		vq_handle = doca_devemu_vnet_ctrl_vq_as_vq(controller->cvq);
		if (!vq_handle) {
			DOCA_LOG_ERR("Failed to get CVQ handle");
			return;
		}
		DOCA_LOG_DBG("Configuring CVQ at index %d", cvq_index);
	} else if (vq_index % 2 == 0 && vq_index < cvq_index) {
		/* Even index = RX VQ - Calculate QP index */
		qp_idx = vq_index / 2;
		if (qp_idx >= controller->max_queue_pairs) {
			DOCA_LOG_ERR("Invalid RX VQ index %d (QP%d, max_qp=%d)",
				     vq_index,
				     qp_idx,
				     controller->max_queue_pairs);
			return;
		}
		if (qp_idx >= atomic_load(&controller->num_active_qps) ||
		    atomic_load(&controller->deferred_mq.initial_data_qps_deferred)) {
			return;
		}
		if (!controller->rx_vqs[qp_idx]) {
			DOCA_LOG_ERR("Invalid RX VQ index %d (QP%d, max_qp=%d)",
				     vq_index,
				     qp_idx,
				     controller->max_queue_pairs);
			return;
		}
		vq_handle = doca_devemu_vnet_rx_vq_as_vq(controller->rx_vqs[qp_idx]);
		if (!vq_handle) {
			DOCA_LOG_ERR("Failed to get RX VQ%d handle", vq_index);
			return;
		}
		DOCA_LOG_DBG("Configuring RX VQ%d (QP%d)", vq_index, qp_idx);
	} else if (vq_index % 2 == 1 && vq_index < cvq_index) {
		/* Odd index = TX VQ - Calculate QP index */
		qp_idx = vq_index / 2;
		if (qp_idx >= controller->max_queue_pairs) {
			DOCA_LOG_ERR("Invalid TX VQ index %d (QP%d, max_qp=%d)",
				     vq_index,
				     qp_idx,
				     controller->max_queue_pairs);
			return;
		}
		if (qp_idx >= atomic_load(&controller->num_active_qps) ||
		    atomic_load(&controller->deferred_mq.initial_data_qps_deferred)) {
			return;
		}
		if (!controller->tx_vqs[qp_idx]) {
			DOCA_LOG_ERR("Invalid TX VQ index %d (QP%d, max_qp=%d)",
				     vq_index,
				     qp_idx,
				     controller->max_queue_pairs);
			return;
		}
		vq_handle = doca_devemu_vnet_tx_vq_as_vq(controller->tx_vqs[qp_idx]);
		if (!vq_handle) {
			DOCA_LOG_ERR("Failed to get TX VQ%d handle", vq_index);
			return;
		}
		DOCA_LOG_DBG("Configuring TX VQ%d (QP%d)", vq_index, qp_idx);
	} else {
		/* Invalid VQ index */
		DOCA_LOG_ERR("Invalid VQ index %d (cvq_index=%d)", vq_index, cvq_index);
		return;
	}

	/* Apply VQ configuration to hardware */
	result = doca_devemu_virtio_vq_set_conf(vq_handle,
						vq_index,
						dev->vqs[vq_index].queue_size,
						dev->vqs[vq_index].queue_msix_vector,
						dev->vqs[vq_index].queue_desc,
						dev->vqs[vq_index].queue_driver,
						dev->vqs[vq_index].queue_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to apply VQ%d configuration: %s", vq_index, doca_error_get_descr(result));
		return;
	}

	DOCA_LOG_INFO("VQ%d configured: size=%u, msix=%u, desc=0x%lx, driver=0x%lx, device=0x%lx",
		      vq_index,
		      dev->vqs[vq_index].queue_size,
		      dev->vqs[vq_index].queue_msix_vector,
		      dev->vqs[vq_index].queue_desc,
		      dev->vqs[vq_index].queue_driver,
		      dev->vqs[vq_index].queue_device);

	common_cfg = vnet_pci_device_get_pci_cfg(dev);
	if (!atomic_load(&controller->engine_enabled) && common_cfg != NULL &&
	    (common_cfg->device_status & VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK) != 0) {
		pci_cfg_workqueue_submit_start_and_enable(controller, dev);
	}
}

/************************************************************************
******                 Core VirtIO Implementation                 ******
************************************************************************/

/*
 * Initialize VirtIO queues
 */
static void vnet_pci_device_queue_init(struct vnet_pci_device *dev)
{
	struct vnet_virtio_common_config *pci_cfg = &dev->pci_cfg;
	int i;

	pci_cfg->queue_select = 0;

	/* Initialize all allocated VQ entries */
	memset(dev->vqs, 0, dev->vqs_count * sizeof(struct vnet_virtio_queue_config));
	for (i = 0; i < dev->vqs_count; i++) {
		dev->vqs[i].queue_size = dev->queue_size; /* Configured queue size */
		dev->vqs[i].queue_msix_vector = VIRTIO_MSI_NO_VECTOR;
		dev->vqs[i].queue_enable = 0;	  /* Disabled by default */
		dev->vqs[i].queue_notify_off = i; /* Notification offset */
	}

	/* Copy current queue to PCI config */
	if (dev->vqs_count > 0)
		memcpy(&pci_cfg->vq, &dev->vqs[0], sizeof(pci_cfg->vq));
}

/*
 * Reset VirtIO device to initial state
 */
static void vnet_pci_device_reset(struct vnet_pci_device *dev)
{
	struct vnet_virtio_common_config *pci_cfg = &dev->pci_cfg;

	DOCA_LOG_DBG("VirtIO device reset [started]");

	/* Reset driver features */
	dev->driver_features = 0;
	pci_cfg->driver_feature = 0;
	pci_cfg->driver_feature_select = 0;

	/* Reset device status to 0 (clean state for driver to start initialization) */
	pci_cfg->device_status = 0;

	/* Reinitialize all queues */
	vnet_pci_device_queue_init(dev);

	DOCA_LOG_INFO("VirtIO device reset [finished]");
}

/*
 * Simple feature selection helper
 */
static uint32_t vnet_pci_dev_feature_select(const int n, const uint64_t ftr)
{
	if (n > 1)
		return 0;

	return n ? ftr >> 32 : ftr & 0xFFFFFFFF;
}

/*
 * VirtIO PCI config write handler
 *
 * This function performs the PCI config write operation synchronously,
 * but offloads VQ config and change callback to an async workqueue.
 */
static void vnet_pci_device_pci_cfg_write32(struct vnet_pci_device *dev,
					    const uint64_t offset,
					    const uint32_t val,
					    const uint32_t wr_mask)
{
	struct vnet_virtio_common_config *pci_cfg = &dev->pci_cfg;
	struct vnet_virtio_common_config prev_cfg;
	uint32_t old_val, new_val;
	void *base = pci_cfg;

	vnet_pci_device_apply_reset_status_release(dev);

	/* Save everything to handle RO fields and selectors */
	memcpy(&prev_cfg, pci_cfg, sizeof(prev_cfg));

	/* Use memcpy for safe unaligned access to packed struct */
	memcpy(&old_val, (uint8_t *)base + offset, sizeof(uint32_t));

	/* Apply write with mask */
	new_val = (old_val & ~wr_mask) | (val & wr_mask);
	memcpy((uint8_t *)base + offset, &new_val, sizeof(uint32_t));
	DOCA_LOG_DBG("pci_cfg+%ld: 0x%x <- 0x%x (val 0x%x & mask 0x%x)", offset, old_val, new_val, val, wr_mask);

	/* Check RO fields and rollback if violated */
	if (prev_cfg.device_feature != pci_cfg->device_feature || prev_cfg.num_queues != pci_cfg->num_queues ||
	    prev_cfg.config_generation != pci_cfg->config_generation ||
	    prev_cfg.admin_queue_index != pci_cfg->admin_queue_index ||
	    prev_cfg.admin_queue_num != pci_cfg->admin_queue_num) {
		DOCA_LOG_ERR("pci_cfg: attempt to overwrite RO field: ROLLBACK");
		goto rollback;
	}

	/* Handle device feature select changes */
	if (prev_cfg.device_feature_select != pci_cfg->device_feature_select) {
		DOCA_LOG_DBG("FEATURE SELECT: device_feature_select %d -> %d",
			     prev_cfg.device_feature_select,
			     pci_cfg->device_feature_select);
		pci_cfg->device_feature =
			vnet_pci_dev_feature_select(pci_cfg->device_feature_select, dev->device_features);
		DOCA_LOG_DBG("DEVICE FEATURES[%d]: 0x%08x (%s bits)",
			     pci_cfg->device_feature_select,
			     pci_cfg->device_feature,
			     (pci_cfg->device_feature_select == 0) ? "lower32" : "upper32");

		if (pci_cfg->device_feature_select == 1 && (dev->device_features & (1ULL << VNET_VIRTIO_F_VERSION_1))) {
			DOCA_LOG_INFO("VIRTIO_F_VERSION_1 now visible to driver (bit 32)");
		}
	}

	/* Handle driver feature select changes */
	if (prev_cfg.driver_feature_select != pci_cfg->driver_feature_select) {
		DOCA_LOG_DBG("DRIVER FEATURE SELECT: %d -> %d",
			     prev_cfg.driver_feature_select,
			     pci_cfg->driver_feature_select);
		uint32_t *p = (uint32_t *)&dev->driver_features;
		if (prev_cfg.driver_feature_select <= 1)
			p[prev_cfg.driver_feature_select] = pci_cfg->driver_feature;

		pci_cfg->driver_feature =
			vnet_pci_dev_feature_select(pci_cfg->driver_feature_select, dev->driver_features);
		DOCA_LOG_DBG("DRIVER FEATURES[%d]: 0x%08x (negotiated=0x%lx)",
			     pci_cfg->driver_feature_select,
			     pci_cfg->driver_feature,
			     dev->driver_features);
	}

	/* Handle driver feature writes (feature negotiation) */
	if (prev_cfg.driver_feature != pci_cfg->driver_feature) {
		DOCA_LOG_DBG("FEATURE NEGOTIATION: driver_feature[%d] = 0x%08x",
			     pci_cfg->driver_feature_select,
			     pci_cfg->driver_feature);
	}

	/* Handle queue select changes (configuring queues) */
	if (prev_cfg.queue_select != pci_cfg->queue_select) {
		DOCA_LOG_DBG("queue select: %d -> %d", prev_cfg.queue_select, pci_cfg->queue_select);

		if (prev_cfg.queue_select < pci_cfg->num_queues)
			memcpy(&dev->vqs[prev_cfg.queue_select], &pci_cfg->vq, sizeof(pci_cfg->vq));

		/* Device Requirements: present 0 in queue_size if virtqueue unavailable */
		if (pci_cfg->queue_select < pci_cfg->num_queues)
			memcpy(&pci_cfg->vq, &dev->vqs[pci_cfg->queue_select], sizeof(pci_cfg->vq));
		else
			memset(&pci_cfg->vq, 0, sizeof(pci_cfg->vq));
	}

	/* VirtIO Spec Compliance: Apply VQ configuration when queue_enable transitions 0→1,
	 * or when an already-enabled queue's addresses become complete.
	 *
	 * IMPORTANT: Use shadow array (dev->vqs[]) for previous state, NOT prev_cfg.vq.
	 * prev_cfg.vq reflects the previously *selected* queue's snapshot, which may be
	 * different from pci_cfg->queue_select if queue_select changed in this write.
	 * The shadow array contains the authoritative last known state for each queue. */
	if (pci_cfg->queue_select < pci_cfg->num_queues) {
		uint16_t vq_idx = pci_cfg->queue_select;
		struct vnet_virtio_queue_config *shadow_vq = &dev->vqs[vq_idx];
		bool prev_enabled = shadow_vq->queue_enable;
		bool curr_enabled = pci_cfg->vq.queue_enable;

		/* Check if queue_enable transitions 0→1 */
		if (!prev_enabled && curr_enabled) {
			DOCA_LOG_DBG("VQ%d enabled (0→1) - applying configuration to hardware", vq_idx);

			/* Update shadow array with current configuration */
			memcpy(shadow_vq, &pci_cfg->vq, sizeof(pci_cfg->vq));

			/* Apply VQ configuration to hardware asynchronously via workqueue
			 * Note: VQs are configured but NOT started yet. They will be started
			 * and enabled when DRIVER_OK is set by the host. */
			pci_cfg_workqueue_submit_vq_config(dev, vq_idx);

		} else if (curr_enabled) {
			/* For already enabled queues, detect when addresses become complete.
			 * Host may set queue_enable before completing address writes. */
			bool was_incomplete = (shadow_vq->queue_desc == 0 || shadow_vq->queue_driver == 0 ||
					       shadow_vq->queue_device == 0);
			bool is_complete = (pci_cfg->vq.queue_desc != 0 && pci_cfg->vq.queue_driver != 0 &&
					    pci_cfg->vq.queue_device != 0);

			/* Update shadow array with current configuration */
			memcpy(shadow_vq, &pci_cfg->vq, sizeof(pci_cfg->vq));

			/* If queue transitioned from incomplete to complete, re-submit vq_config */
			if (was_incomplete && is_complete) {
				DOCA_LOG_INFO("VQ%d addresses now complete - re-submitting vq_config", vq_idx);
				pci_cfg_workqueue_submit_vq_config(dev, vq_idx);
			} else {
				DOCA_LOG_DBG("Updated VQ%d config in shadow array (queue already enabled)", vq_idx);
			}
		}
	}

	/* Handle device status changes */
	if (prev_cfg.device_status != pci_cfg->device_status) {
		uint8_t prev_status = prev_cfg.device_status;
		uint8_t status = pci_cfg->device_status;

		DOCA_LOG_INFO("VIRTIO STATUS CHANGE: 0x%02x -> 0x%02x", prev_status, status);

		if (status == 0) {
			DOCA_LOG_INFO("VirtIO Driver: DEVICE RESET - reinitializing device");

			/* Signal cancellation BEFORE the barrier wait.
			 * Long-running worker operations (e.g., start_additional_queue_pairs)
			 * check this flag in their loops and bail out early. This bounds
			 * the barrier wait to one sub-operation (~25-50ms) instead of the
			 * full duration (3-7s for 64-127 QPs), preventing PCIe Completion
			 * Timeout on the host side. */
			atomic_store(&dev->cancel_in_progress, true);
			atomic_fetch_add(&dev->reset_generation, 1);

			/* Per-device reset barrier: only wait if worker is busy with THIS device.
			 *
			 * The original global barrier (wait for worker_busy=false) caused a
			 * deadlock during back-to-back hotplug:
			 *   Main thread: PE1 callback -> DEVICE_RESET -> waits for worker idle
			 *   Worker: create_device() for ANOTHER DSP (worker_busy=true, 350ms)
			 *   -> PE1 blocked in callback -> worker can't complete -> deadlock
			 *
			 * Per-device fix: track which device the worker is operating on.
			 * Only wait when worker_current_dev matches the device being reset.
			 * - Hotplug for other DSP: worker_current_dev=NULL != dev -> passes
			 * - VQ config for other DSP: different dev pointer -> passes
			 * - VQ config for same DSP: same dev pointer -> waits briefly
			 *   (with cancel_in_progress set, long ops exit fast) */
			if (pci_cfg_wq != NULL) {
				pthread_mutex_lock(&pci_cfg_wq->mutex);
				pci_cfg_wq->reset_in_progress = true;
				/* Wait only if worker is busy with the SAME device */
				while (pci_cfg_wq->worker_busy && pci_cfg_wq->worker_current_dev == dev) {
					DOCA_LOG_DBG("Reset waiting for worker to finish same-device work");
					pthread_cond_wait(&pci_cfg_wq->reset_cond, &pci_cfg_wq->mutex);
				}
				pthread_mutex_unlock(&pci_cfg_wq->mutex);
			}

			vnet_pci_device_reset(dev);

			if (pci_cfg_wq != NULL) {
				pthread_mutex_lock(&pci_cfg_wq->mutex);
				pci_cfg_wq->reset_in_progress = false;
				pthread_cond_broadcast(&pci_cfg_wq->reset_cond); /* Wake waiting worker */
				pthread_mutex_unlock(&pci_cfg_wq->mutex);
			}
		} else {
			/* Only check newly set bit */
			switch (status & ~prev_status) {
			case VNET_VIRTIO_DEVICE_STATUS_ACK:
				DOCA_LOG_INFO("VirtIO Driver: ACKNOWLEDGE - driver detected VirtIO device");
				break;
			case VNET_VIRTIO_DEVICE_STATUS_DRIVER:
				DOCA_LOG_INFO("VirtIO Driver: DRIVER loaded - OS driver attached to device");
				break;
			case VNET_VIRTIO_DEVICE_STATUS_FEATURES_OK:
				DOCA_LOG_INFO("VirtIO Driver: FEATURES_OK - feature negotiated (=0x%lx)",
					      dev->driver_features);
				DOCA_LOG_DBG("VirtIO Spec Compliance: Hardware preparation will be triggered");
				break;
			case VNET_VIRTIO_DEVICE_STATUS_DRIVER_OK:
				DOCA_LOG_INFO("VirtIO Driver: DRIVER_OK - device should already be live!");
				DOCA_LOG_DBG("VirtIO Spec Compliant: Hardware was prepared during FEATURES_OK phase");

				/* Save the currently selected VQ configuration for completeness */
				if (pci_cfg->queue_select < pci_cfg->num_queues) {
					memcpy(&dev->vqs[pci_cfg->queue_select], &pci_cfg->vq, sizeof(pci_cfg->vq));
					DOCA_LOG_DBG("Saved final VQ%d config: enable=%d, size=%d, desc=0x%lx",
						     pci_cfg->queue_select,
						     pci_cfg->vq.queue_enable,
						     pci_cfg->vq.queue_size,
						     pci_cfg->vq.queue_desc);
				}

				DOCA_LOG_INFO(
					"Final State: features=0x%lx, queues=%d, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
					dev->driver_features,
					pci_cfg->num_queues,
					dev->vnet_cfg.mac[0],
					dev->vnet_cfg.mac[1],
					dev->vnet_cfg.mac[2],
					dev->vnet_cfg.mac[3],
					dev->vnet_cfg.mac[4],
					dev->vnet_cfg.mac[5]);
				break;
			case VNET_VIRTIO_DEVICE_STATUS_FAILED:
				DOCA_LOG_ERR("VirtIO Driver: FAILED - driver encountered an error");
				break;
			default:
				DOCA_LOG_DBG("??? unknown device status: 0x%02x", status & ~prev_status);
				break;
			}
		}
	}

	/* Notify configuration change synchronously */
	if (dev->pci_cfg_change_cb)
		dev->pci_cfg_change_cb(dev, dev->cb_arg);
	if (atomic_load(&dev->cancel_in_progress) && pci_cfg->device_status != VNET_VIRTIO_DEVICE_STATUS_NEEDS_RESET &&
	    !vnet_pci_device_reset_status_is_pending(dev))
		atomic_store(&dev->cancel_in_progress, false);
	return;

rollback:
	memcpy(pci_cfg, &prev_cfg, sizeof(prev_cfg));
}

/*
 * VirtIO MMIO write handler
 */
static void vnet_pci_device_mmio_write32(struct vnet_pci_device *dev,
					 const uint64_t addr,
					 const uint32_t val,
					 const uint32_t wr_mask)
{
	uint64_t offset = vnet_pci_dev_bar_offset(dev, addr);

	DOCA_LOG_DBG("mmio write offset 0x%lx value 0x%08x", offset, val);

	if (offset < sizeof(dev->pci_cfg)) {
		/* Handle VirtIO common configuration writes - bounds checked against actual struct size */
		vnet_pci_device_pci_cfg_write32(dev, offset, val, wr_mask);
	} else if (offset >= VNET_VIRTIO_DEV_CFG_OFFSET &&
		   offset < VNET_VIRTIO_DEV_CFG_OFFSET + sizeof(dev->vnet_cfg)) {
		offset -= VNET_VIRTIO_DEV_CFG_OFFSET;
		/* Additional bounds check for device config writes */
		if (offset + sizeof(uint32_t) > sizeof(dev->vnet_cfg)) {
			DOCA_LOG_ERR("mmio write - offset 0x%lx + 4 bytes (1DW) exceeds vnet_cfg boundary (size: %zu)",
				     offset,
				     sizeof(dev->vnet_cfg));
			return;
		}
		/* Device config writes - typically read-only for network devices */
		DOCA_LOG_DBG("dev_cfg write offset 0x%lx value 0x%08x (read-only)", offset, val);
	} else {
		DOCA_LOG_ERR("mmio write drop - unsupported virtio bar offset 0x%lx", offset);
		return;
	}
}

/*
 * VirtIO MMIO read handler
 */
static uint32_t vnet_pci_device_mmio_read32(struct vnet_pci_device *dev, const uint64_t addr)
{
	uint64_t offset = vnet_pci_dev_bar_offset(dev, addr);
	size_t struct_size;
	uint32_t value;
	void *base;

	DOCA_LOG_DBG("mmio read offset 0x%lx", offset);

	if (offset < sizeof(dev->pci_cfg)) {
		/* PCI Common Configuration region - bounds checked against actual struct size */
		vnet_pci_device_apply_reset_status_release(dev);
		base = &dev->pci_cfg;
	} else if (offset >= VNET_VIRTIO_DEV_CFG_OFFSET &&
		   offset < VNET_VIRTIO_DEV_CFG_OFFSET + sizeof(dev->vnet_cfg)) {
		/* Device-specific configuration region */
		base = &dev->vnet_cfg;
		offset -= VNET_VIRTIO_DEV_CFG_OFFSET;
	} else {
		DOCA_LOG_ERR("mmio read - unsupported virtio bar offset 0x%lx", offset);
		return 0xFAFAFAFA;
	}

	/* Additional bounds check to ensure we don't read past the end of the struct */
	struct_size = (base == &dev->pci_cfg) ? sizeof(dev->pci_cfg) : sizeof(dev->vnet_cfg);
	if (offset + sizeof(uint32_t) > struct_size) {
		DOCA_LOG_ERR("mmio read - offset 0x%lx + 4 bytes (1DW) exceeds struct boundary (size: %zu)",
			     offset,
			     struct_size);
		return 0xFAFAFAFA;
	}

	/* Use memcpy for safe unaligned access to packed struct */
	memcpy(&value, (uint8_t *)base + offset, sizeof(uint32_t));
	return value;
}
