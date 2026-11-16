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

#ifndef MEMOS_INITIATOR_REF_APP_CORE_H_
#define MEMOS_INITIATOR_REF_APP_CORE_H_

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_type.h>
#include <doca_devemu_nvme.h>
#include <doca_devemu_nvme_dev.h>
#include <doca_devemu_nvme_cq.h>
#include <doca_devemu_nvme_sq.h>
#include <doca_devemu_nvme_io.h>
#include <doca_devemu_nvme_type.h>
#include <doca_error.h>
#include <doca_mmap.h>
#include <doca_pe.h>

/* NVMe BAR0 controller register offsets (NVMe spec). Shared by memos_initiator_ref_app_core.c (reg_change
 * logging + CC/CSTS mirroring) and memos_initiator_ref_app_admin.c (admin queue bring-up). */
#define NVME_REG_CAP 0x00
#define NVME_REG_VS 0x08
#define NVME_REG_INTMS 0x0C
#define NVME_REG_INTMC 0x10
#define NVME_REG_CC 0x14
#define NVME_REG_CSTS 0x1C
#define NVME_REG_AQA 0x24
#define NVME_REG_ASQ 0x28
#define NVME_REG_ACQ 0x30

/* Reset-default CAP/VS the controller advertises until a host driver acts (both are read-only to
 * the host). The app owns these: it installs them as the pci_type BAR0 defaults during hotplug and
 * live-seeds them once the controller is RUNNING. */
#define MEMOS_INITIATOR_REF_APP_CAP_MQES_MASK 0xFFFFULL	  /* MQES occupies CAP bits [15:0], zero-based. */
#define MEMOS_INITIATOR_REF_APP_CAP_CQR_BIT (1ULL << 16)  /* Contiguous queues required. */
#define MEMOS_INITIATOR_REF_APP_CAP_TO_15 (0x0FULL << 24) /* TO = 15 (15 * 500 ms timeout). */
#define MEMOS_INITIATOR_REF_APP_CAP_CSS_NVM (1ULL << 37)  /* NVM command set supported. */
/* CAP.CSS occupies bits [44:37]; bit 6 of that field is bit 43. It is what lets a host select
 * CC.CSS=110b, the only mode in which a namespace may report a CSI other than NVM. */
#define MEMOS_INITIATOR_REF_APP_CAP_CSS_IO (1ULL << 43)
/* Fixed CAP bits (everything except the device-dependent MQES field). */
#define MEMOS_INITIATOR_REF_APP_CAP_FIXED_BITS \
	(MEMOS_INITIATOR_REF_APP_CAP_CQR_BIT | MEMOS_INITIATOR_REF_APP_CAP_TO_15 | \
	 MEMOS_INITIATOR_REF_APP_CAP_CSS_NVM | MEMOS_INITIATOR_REF_APP_CAP_CSS_IO)
/* Build a CAP value advertising 'mqes' (zero-based, == max I/O queue entries - 1). */
#define MEMOS_INITIATOR_REF_APP_CAP_FROM_MQES(mqes) \
	(((uint64_t)(mqes)&MEMOS_INITIATOR_REF_APP_CAP_MQES_MASK) | MEMOS_INITIATOR_REF_APP_CAP_FIXED_BITS)
/* PCI identification for the emulated NVMe function. Firmware fingerprints a generic pci_type by
 * (name, content), so a type of this name must always be created with these same values. */
#define MEMOS_INITIATOR_REF_APP_PCI_TYPE_NAME "DOCA_DEVEMU_NVME"
#define MEMOS_INITIATOR_REF_APP_PCI_VENDOR_ID 0x15b3
#define MEMOS_INITIATOR_REF_APP_PCI_DEVICE_ID 0x6001
#define MEMOS_INITIATOR_REF_APP_PCI_SUBSYSTEM_VENDOR_ID 0x15b3
#define MEMOS_INITIATOR_REF_APP_PCI_SUBSYSTEM_ID 0x0051
#define MEMOS_INITIATOR_REF_APP_PCI_REVISION_ID 0
#define MEMOS_INITIATOR_REF_APP_PCI_CLASS_CODE 0x010802 /* NVMe storage controller. */
#define MEMOS_INITIATOR_REF_APP_PCI_NUM_MSIX 32
#define MEMOS_INITIATOR_REF_APP_PCI_NUM_DB 64

#define MEMOS_INITIATOR_REF_APP_DEFAULT_VS 0x00010400u /* NVMe 1.4.0. */

#define NVME_CC_EN_MASK 0x1u
/* CC.SHN (Shutdown Notification, bits [15:14]): 0=none, 1=normal, 2=abrupt. */
#define NVME_CC_SHN_SHIFT 14
#define NVME_CC_SHN_MASK (0x3u << NVME_CC_SHN_SHIFT)
#define NVME_CSTS_RDY_MASK 0x1u
#define NVME_CSTS_CFS_MASK 0x2u
/* CSTS.SHST (Shutdown Status, bits [3:2]): 0=normal, 1=processing, 2=complete. */
#define NVME_CSTS_SHST_SHIFT 2
#define NVME_CSTS_SHST_MASK (0x3u << NVME_CSTS_SHST_SHIFT)
#define NVME_CSTS_SHST_COMPLETE (0x2u << NVME_CSTS_SHST_SHIFT)

/* App policy caps. MEMOS_INITIATOR_REF_APP_MAX_IO_QUEUES_HARD is the absolute upper bound on IO queue pairs,
 * independent of the --core-mask / --num-io-queues-per-core runtime configuration. It is bounded by
 * the firmware resources the pci_type advertises: 32 MSI-X vectors (vector 0 is the admin CQ, so up
 * to 31 IO CQs can each own a vector) and 64 doorbells (qid n consumes db 2n and 2n+1, so qid <= 31).
 * The runtime advertised count (app->max_io_queues) is clamped to this. MEMOS_INITIATOR_REF_APP_MAX_HELD_AER
 * bounds how many Async-Event-Request commands we hold uncompleted (host respects the advertised
 * AERL; the cap is a safety net). */
#define MEMOS_INITIATOR_REF_APP_MAX_IO_QUEUES_HARD 31
#define MEMOS_INITIATOR_REF_APP_MAX_HELD_AER 8

/* Per-request user-data buffer the app asks the library to allocate for each NVMe request. Used
 * here only to exercise the feature (the dispatch handler stamps a marker into it). */
#define MEMOS_INITIATOR_REF_APP_REQ_USER_DATA_SIZE 64

/* Default and maximum for --io-defer-max-us: how long an IO completion held by --io-defer-count may
 * be withheld before it is released anyway. The default keeps a partially-filled batch from stalling
 * a host that stopped submitting; reproducing an SQ-tail wrap needs a much larger value (the tail has
 * to advance a whole queue depth), hence the generous ceiling - still well under a host's command
 * timeout. */
#define MEMOS_INITIATOR_REF_APP_IO_DEFER_MAX_US 1000		/* 1 ms */
#define MEMOS_INITIATOR_REF_APP_IO_DEFER_MAX_US_LIMIT 10000000u /* 10 s */

/* The single namespace this app exposes: NSID 1, command set KV.
 *
 * It answers the NVM-flavoured Identify Namespace (CNS 00h) with 512 B LBAs over 128 MiB because
 * that is where a host - and libdoca_kv's query_ns_nguid() - reads the NGUID from. The KV geometry a
 * KV client acts on comes from CNS 05h. */
#define MEMOS_INITIATOR_REF_APP_NS_NSID 1
#define MEMOS_INITIATOR_REF_APP_NS_LBA_SHIFT 9
#define MEMOS_INITIATOR_REF_APP_NS_NUM_BLOCKS ((uint64_t)128 * 1024 * 1024 >> MEMOS_INITIATOR_REF_APP_NS_LBA_SHIFT)

/* The transport the initiator connects over. The target itself is named by runtime parameters. */
#define MEMOS_INITIATOR_REF_APP_TRTYPE_RDMA 0x01 /* NVMF_TRTYPE_RDMA */
#define MEMOS_INITIATOR_REF_APP_ADRFAM_IPV4 0x01 /* NVMF_ADDR_FAMILY_IP4 */

/* Bounds on the target-identifying strings. NQNs are capped by the NVMe spec at 223 bytes. */
#define MEMOS_INITIATOR_REF_APP_NQN_SIZE 224
#define MEMOS_INITIATOR_REF_APP_TRADDR_SIZE 64
#define MEMOS_INITIATOR_REF_APP_TRSVCID_SIZE 32

/* Longest key the namespace accepts; the KV command set cannot carry more than 16 B in the SQE. */
#define MEMOS_INITIATOR_REF_APP_KV_MAX_KEY_LEN 16
/* Longest value the namespace accepts. Equal to MEMOS_INITIATOR_REF_APP_DMA_SLOT_SIZE so one value
 * is one scratch slot, and within the two pages MDTS=1 allows so the DMA helper can reject PRP
 * lists outright. */
#define MEMOS_INITIATOR_REF_APP_KV_MAX_VALUE_LEN 4096
/* One would do, since commands are serialized, but a small pool costs nothing. */
#define MEMOS_INITIATOR_REF_APP_KV_NUM_TASKS 8

/* Reconnect backoff, and the tenant this application's volume belongs to. */
#define MEMOS_INITIATOR_REF_APP_KV_RECONNECT_DELAY_SEC 2
#define MEMOS_INITIATOR_REF_APP_KV_TENANT_ID 0

/* The only bounded wait: reaching a target that may not be there. Everything after it waits on an
 * event the initiator is committed to delivering, so those waits are unbounded. */
#define MEMOS_INITIATOR_REF_APP_KV_CONNECT_TIMEOUT_US (30 * 1000 * 1000) /* 30 s */

/* AQA fields (both 0-based per spec: number of entries = field + 1). */
#define NVME_AQA_ASQS(aqa) ((aqa)&0xFFFu)
#define NVME_AQA_ACQS(aqa) (((aqa) >> 16) & 0xFFFu)

/* Opaque DMA engine used to move payloads between controller-local scratch and host PRP memory.
 * Defined in memos_initiator_ref_app_dma.c. */
struct memos_initiator_ref_app_dma;

/* Opaque key/value store backing the KV namespace. Defined in memos_initiator_ref_app_kv.c. */
struct memos_initiator_ref_app_kv;

/* Opaque qid-indexed registry of host-created IO Completion/Submission queues. Defined in
 * memos_initiator_ref_app_io.c. */
struct memos_initiator_ref_app_ioq_registry;

/* Per-core IO datapath worker (own PE + io ctx + datapath worker + pinned thread). Defined in
 * memos_initiator_ref_app_worker.c. */
struct memos_initiator_ref_app_worker;

/**
 * @brief PCI type the app uses for hotplugged functions (--pci-type).
 *
 * Selects where the NVMe BAR0 map comes from. The two generic-emulation kinds work on firmware
 * without native NVMe emulation; the canned one additionally needs the generic emulation manager to
 * offer the NVMe BAR layout (doca_devemu_nvme_is_pci_type_supported()). The native kind needs
 * firmware that emulates NVMe itself, and there BAR0 is firmware's rather than ours.
 */
enum memos_initiator_ref_app_pci_type_kind {
	/** doca_devemu_nvme_pci_type_create(): firmware-canned NVMe BAR layout. */
	MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_GENERIC_NVME = 0,
	/** doca_devemu_pci_type_create(): app-supplied NVMe BAR layout. */
	MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_GENERIC,
	/** doca_devemu_nvme_find_default_nvme_pci_type_by_dev(): native NVMe emulation. */
	MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_NATIVE,
};

/**
 * @brief CLI configuration parsed from argp.
 */
struct memos_initiator_ref_app_cfg {
	char pci_address[DOCA_DEVINFO_PCI_ADDR_SIZE];
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	/* The NVMe-oF target backing the KV namespace (--traddr / --trsvcid / --subnqn / --hostnqn).
	 * All four are mandatory and none has a built-in default: the target is a property of the
	 * fabric the application is deployed on, so the same binary must serve any of them. */
	/* ibdev name of the RDMA device with a route to the target (--rdma-dev). Distinct from
	 * --pci-addr: that names the NVMe emulation manager facing the host, while this names the
	 * fabric port facing the target, and on a BlueField they are different devices. */
	char rdma_dev[DOCA_DEVINFO_IBDEV_NAME_SIZE];
	char traddr[MEMOS_INITIATOR_REF_APP_TRADDR_SIZE];
	char trsvcid[MEMOS_INITIATOR_REF_APP_TRSVCID_SIZE];
	char subnqn[MEMOS_INITIATOR_REF_APP_NQN_SIZE];
	char hostnqn[MEMOS_INITIATOR_REF_APP_NQN_SIZE];
	char hotunplug_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	bool list_emulated_functions;
	bool hotplug;
	bool hotunplug;
	/* Where the BAR0 map of the app-owned pci_type comes from (--pci-type, default generic_nvme). */
	enum memos_initiator_ref_app_pci_type_kind pci_type_kind;
	/* Number of IO queues handled per worker core (--num-io-queues-per-core, default 1). */
	uint32_t num_io_queues_per_core;
	/* Debug/test: number of IO completions to hold back before releasing them as a batch
	 * (--io-defer-count, 0 = complete inline). Exercises delayed completion. */
	uint32_t io_defer_count;
	/* Debug/test: number of oldest requests to keep held when a batch is released
	 * (--io-defer-keep, default 0 = release the whole batch). Keeping a request held while newer
	 * ones complete lets the host's SQ tail wrap around onto the held command's slot, which is
	 * what exercises per-slot request bookkeeping. Must be < io_defer_count. */
	uint32_t io_defer_keep;
	/* Debug/test: how long a held request may be kept before it is released regardless
	 * (--io-defer-max-us). Bounds the kept requests from above so the host cannot time out. */
	uint32_t io_defer_max_us;
	/* Debug/test: release a held batch newest-first instead of oldest-first
	 * (--io-defer-reverse), so a later command completes before the earlier ones it arrived
	 * with. Exercises out-of-order completion. */
	bool io_defer_reverse;
	/* CPU bitmask of cores that run IO datapath worker threads (--core-mask, default 0x1). One
	 * worker thread is spawned and pinned per set bit. */
	uint64_t core_mask;
};

/**
 * @brief Runtime state of the MEMOS initiator reference application.
 *
 * Owned by main(), populated by memos_initiator_ref_app_run().
 */
struct memos_initiator_ref_app_resources {
	struct memos_initiator_ref_app_cfg cfg;
	/* Control-plane PE: drives the nvme_dev ctx, i.e. BAR / register-change (CC, shutdown, FLR)
	 * events. Polled by the main thread. */
	struct doca_pe *pe;
	/* Main-thread admin PE: the admin io ctx (qid 0) and the admin payload DMA engine connect to
	 * it; it owns the admin queues' datapath worker. Polled by the main thread alongside pe. The IO
	 * datapath lives on the per-core worker threads, not here. NULL on the list/hotplug-only paths. */
	struct doca_pe *admin_pe;
	struct doca_dev *dev;
	struct doca_dev_rep *rep;
	/* True when 'rep' is a firmware-owned static function rather than one we hotplugged, read from
	 * the representor once it is open. Static functions are already powered on and carry a
	 * firmware-configured BAR0, so we neither power them on/off nor seed their registers. */
	bool rep_is_static;
	/* App-owned NVMe pci_type (created/configured/started in create_nvme_pci_type(), released in
	 * destroy_nvme_pci_type()). */
	struct doca_devemu_pci_type *pci_type;
	struct doca_devemu_nvme_dev *nvme_dev;
	struct doca_mmap *host_mmap;
	/* Admin queues (qid 0), brought up on CC.EN 0->1 and torn down on reset/exit. The SQ posts
	 * its completions to the admin CQ, so it is created after and destroyed before the CQ. */
	struct doca_devemu_nvme_cq *admin_cq;
	struct doca_devemu_nvme_sq *admin_sq;
	/* The single io ctx shared by the admin SQ and every IO SQ. The registered dispatcher routes
	 * requests to the admin or IO handler by the SQ binding user data. Created/destroyed with the
	 * admin queues, but kept up across IO-queue create/delete. */
	struct doca_devemu_nvme_io *admin_io;
	/* Registry of host-created IO queues; allocated after the io ctx is up, freed before it stops. */
	struct memos_initiator_ref_app_ioq_registry *ioq;
	/* Per-core IO datapath workers, one per set bit in cfg.core_mask. Created/started in
	 * memos_initiator_ref_app_workers_start() (after nvme_dev is RUNNING) and joined/destroyed in
	 * memos_initiator_ref_app_workers_stop(). IO queues are bound to these (round-robin by qid), not to admin_io.
	 */
	struct memos_initiator_ref_app_worker *workers;
	uint32_t num_workers;
	/* Advertised IO queue pairs = num_workers * cfg.num_io_queues_per_core, clamped to
	 * MEMOS_INITIATOR_REF_APP_MAX_IO_QUEUES_HARD. Drives Set/Get-Features NUMBER_OF_QUEUES and the IO registry. */
	uint32_t max_io_queues;
	/* Max I/O queue depth in ENTRIES (1-based) the controller accepts, queried from the device via
	 * doca_devemu_nvme_cap_get_max_io_queue_size() (mqes + 1) and clamped to
	 * MEMOS_INITIATOR_REF_APP_MAX_QUEUE_SIZE. Bounds the Create-IO-CQ/SQ qsize and the advertised CAP.MQES. Falls
	 * back to MEMOS_INITIATOR_REF_APP_DEFAULT_QUEUE_SIZE if the capability query fails. */
	uint32_t max_io_queue_size;
	/* Async-Event-Request commands are held uncompleted (this app generates no events); they are
	 * completed with an aborted status on controller reset / teardown so the io ctx can drain. */
	struct doca_devemu_nvme_req *held_aers[MEMOS_INITIATOR_REF_APP_MAX_HELD_AER];
	uint32_t num_held_aers;
	/* Async Event Configuration (Set/Get-Features 0x0B). Stored and echoed back only; this app
	 * generates no asynchronous events. */
	uint32_t aen_config;
	/* DMA engine for admin payload delivery to host PRP memory (Identify, Get-Log-Page). Brought
	 * up after the host mmap is cached and torn down before it is destroyed. */
	struct memos_initiator_ref_app_dma *dma;
	/* The KV namespace's key/value store, shared by every IO worker. Brought up before the workers
	 * start and torn down after they stop. */
	struct memos_initiator_ref_app_kv *kv;
	/* Set when the host driver already had the controller enabled (CC.EN=1) at attach time. We
	 * cannot adopt the host's in-flight queues (recovery is out of scope for this app), so we
	 * assert CSTS.CFS and refuse queue bring-up until we observe a clean controller reset
	 * (CC.EN 1->0); only then do we accept the next CC.EN 0->1 enable. */
	bool awaiting_reset;
	/* True while the admin (and IO) queues are brought up. This - not the CSTS.RDY register - is
	 * the source of truth for the enable/disable transition: an FLR resets the whole BAR0 stateful
	 * region (CC and CSTS together), so a CSTS.RDY-based check would miss the disable edge and
	 * leave the queues bound to stale host addresses, causing DMA/IOMMU faults. */
	bool admin_up;
	/* Set by the FLR event handler (runs on the control PE) and consumed by the main control loop,
	 * which drives the ctx stop/start recovery. An FLR is host-driven and asynchronous, so recovery
	 * must not run inline in the handler; it is deferred to the loop. */
	bool flr_pending;
	volatile sig_atomic_t *force_quit;
	doca_error_t error;
};

/**
 * @brief Progress both main-thread PEs (control + admin) once.
 *
 * Used by every main-thread bring-up / teardown wait loop so that contexts living on either main
 * PE (nvme_dev/pci_dev on app->pe; admin io ctx and admin DMA on app->admin_pe) advance regardless
 * of which PE drives the awaited transition. NULL-safe: on the list / hotplug-only paths admin_pe
 * is never created. The per-core IO worker PEs are driven by their own threads, so they are NOT
 * pumped here; IO queue lifecycle drains pump the specific worker PE under a quiesce instead.
 *
 * @param app  application state.
 * @return total number of events processed across both PEs.
 */
static inline int memos_initiator_ref_app_progress_all(struct memos_initiator_ref_app_resources *app)
{
	int worked = 0;

	if (app->admin_pe != NULL)
		worked += doca_pe_progress(app->admin_pe);
	worked += doca_pe_progress(app->pe);
	return worked;
}

/**
 * @brief Main entry into the NVMe controller lifecycle.
 *
 * Brings up the PCI type, the NVMe device, the host mmap, registers the
 * register-change callback, starts the doca_ctx and runs the progress loop
 * until *force_quit becomes non-zero. All resources are released before return.
 *
 * @param app  application state. Caller must populate cfg and force_quit.
 * @return DOCA_SUCCESS on clean shutdown, an error otherwise.
 */
doca_error_t memos_initiator_ref_app_run(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Enumerate every emulated representor managed by the given PCI address.
 *
 * Opens the emulation manager identified by app->cfg.pci_address and calls
 * doca_devinfo_rep_create_list() with the EMULATED filter, so the listing is
 * name-agnostic and type-agnostic: NVMe, generic-PCI, virtio_*, TLP - all
 * emulated reps the device manages are returned in one flat list. For each
 * rep we print VUID, host-side PCI address, PF/VF/SF kind, hotplug status,
 * and a best-effort emulation-type tag inferred from the VUID convention.
 *
 * @param app  application state. Caller must populate cfg.pci_address.
 * @return DOCA_SUCCESS on success, an error otherwise.
 */
doca_error_t memos_initiator_ref_app_list_emulated_functions(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Hotplug a new emulated PCI function under the app's pci_type.
 *
 * Opens the emulation manager, builds and starts the app pci_type, then
 * allocates a new rep via doca_devemu_pci_type_create_rep(), creates a
 * doca_devemu_pci_dev for it, calls doca_devemu_pci_dev_hotplug() and waits
 * (polling the PE) for the device to transition to POWER_ON. The
 * firmware-assigned VUID of the new rep is logged before transition so the
 * user can immediately drive the controller against it with --vuid.
 *
 * @param app  application state. Caller must populate cfg.pci_address and force_quit.
 * @return DOCA_SUCCESS on success, an error otherwise.
 */
doca_error_t memos_initiator_ref_app_hotplug(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Hotunplug an existing emulated PCI function and destroy its rep.
 *
 * Opens the emulation manager, builds and starts the app pci_type, finds the
 * rep matching cfg.hotunplug_vuid, creates a doca_devemu_pci_dev for it,
 * calls doca_devemu_pci_dev_hotunplug() and waits (polling the PE) for the
 * device to transition to POWER_OFF, then destroys the rep so the firmware
 * slot is released.
 *
 * @param app  application state. Caller must populate cfg.pci_address,
 *             cfg.hotunplug_vuid and force_quit.
 * @return DOCA_SUCCESS on success, an error otherwise.
 */
doca_error_t memos_initiator_ref_app_hotunplug(struct memos_initiator_ref_app_resources *app);

#endif /* MEMOS_INITIATOR_REF_APP_CORE_H_ */
