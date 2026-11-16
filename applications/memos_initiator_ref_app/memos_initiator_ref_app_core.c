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

#include "memos_initiator_ref_app_core.h"
#include "memos_initiator_ref_app_admin.h"
#include "memos_initiator_ref_app_dma.h"
#include "memos_initiator_ref_app_io.h"
#include "memos_initiator_ref_app_kv.h"
#include "memos_initiator_ref_app_worker.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_devemu_pci.h>
#include <doca_devemu_pci_type.h>
#include <doca_devemu_pci_info.h>
#include <doca_devemu_nvme.h>
#include <doca_devemu_nvme_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <doca_types.h>

DOCA_LOG_REGISTER(MEMOS_INITIATOR_REF_APP_CORE);

/* NVMe BAR0 register offsets and CC/CSTS masks are shared via memos_initiator_ref_app_core.h. */

#define MEMOS_INITIATOR_REF_APP_PROGRESS_SLEEP_NS (10 * 1000)	      /* 10 us */
#define MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US (10 * 1000 * 1000) /* 10 s */

/* Drives the emulated function's hotplug state in the regular run path; defined after
 * wait_hotplug_state() but used earlier by teardown(). */
static doca_error_t power_off_function(struct memos_initiator_ref_app_resources *app);

/* Stop + destroy the app-owned pci_type; defined alongside create_nvme_pci_type() but used earlier
 * by the teardown paths. */
static void destroy_nvme_pci_type(struct memos_initiator_ref_app_resources *app);

/* Move the NVMe device between DISABLED and ENABLED; defined after stop_ctx_and_wait_idle() but used
 * earlier by handle_cc_write(). */
static doca_error_t nvme_dev_enable(struct memos_initiator_ref_app_resources *app);
static doca_error_t nvme_dev_disable_and_wait(struct memos_initiator_ref_app_resources *app);

static doca_error_t open_emulation_manager_dev(const char *pci_address, struct doca_dev **dev_out)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs;
	doca_error_t result;
	uint8_t is_equal;
	uint8_t is_supported;
	size_t i;

	*dev_out = NULL;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enumerate DOCA devices: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < nb_devs; ++i) {
		if (doca_devinfo_is_equal_pci_addr(dev_list[i], pci_address, &is_equal) != DOCA_SUCCESS ||
		    is_equal == 0)
			continue;

		is_supported = 0;
		result = doca_devemu_pci_cap_is_type_supported(dev_list[i], &is_supported);
		if (result != DOCA_SUCCESS || is_supported == 0) {
			DOCA_LOG_ERR(
				"Device '%s' is present but does not support generic-PCI emulation. Run on a PF with root permissions and a firmware build that exposes the generic PCI emulation manager capability (libdoca_devemu_nvme builds on top of generic emulation).",
				pci_address);
			result = DOCA_ERROR_NOT_SUPPORTED;
			goto out;
		}

		result = doca_dev_open(dev_list[i], dev_out);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to open device '%s': %s", pci_address, doca_error_get_descr(result));
		goto out;
	}

	DOCA_LOG_ERR("No DOCA device found with PCI address '%s'", pci_address);
	result = DOCA_ERROR_NOT_FOUND;

out:
	doca_devinfo_destroy_list(dev_list);
	return result;
}

/* Walks the rep list of pci_type and opens the rep matching the given vuid. The per-pci_type
 * filter only matches reps that were hotplugged under this exact pci_type name, which is what
 * we want when driving the controller (memos_initiator_ref_app_run): a rep created with a different name
 * would not be a valid NVMe controller surface for this app. */
static doca_error_t open_emulated_rep(struct doca_devemu_pci_type *pci_type,
				      const char *vuid,
				      struct doca_dev_rep **rep_out)
{
	struct doca_devinfo_rep **rep_list;
	uint32_t nb_reps;
	uint32_t i;
	char actual_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	doca_error_t result;

	*rep_out = NULL;

	result = doca_devemu_pci_type_create_rep_list(pci_type, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enumerate emulated representors: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < nb_reps; ++i) {
		result = doca_devinfo_rep_get_vuid(rep_list[i], actual_vuid, sizeof(actual_vuid));
		if (result != DOCA_SUCCESS || strncmp(actual_vuid, vuid, DOCA_DEVINFO_REP_VUID_SIZE) != 0)
			continue;

		result = doca_dev_rep_open(rep_list[i], rep_out);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to open rep '%s': %s", vuid, doca_error_get_descr(result));
		goto out;
	}

	DOCA_LOG_ERR("No emulated NVMe representor found with VUID '%s'", vuid);
	result = DOCA_ERROR_NOT_FOUND;

out:
	doca_devinfo_rep_destroy_list(rep_list);
	return result;
}

/* Opens an emulated rep by VUID without filtering by pci_type name. Used by --hotunplug so
 * the user can remove a rep that was hotplugged under any pci_type (e.g. a stale rep left
 * behind by an older library/app version that used a different pci_type name). */
static doca_error_t open_emulated_rep_any(struct doca_dev *emu_mgr, const char *vuid, struct doca_dev_rep **rep_out)
{
	struct doca_devinfo_rep **rep_list = NULL;
	uint32_t nb_reps = 0;
	uint32_t i;
	char actual_vuid[DOCA_DEVINFO_REP_VUID_SIZE];
	doca_error_t result;

	*rep_out = NULL;

	result = doca_devinfo_rep_create_list(emu_mgr, DOCA_DEVINFO_REP_FILTER_EMULATED, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enumerate emulated reps: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < nb_reps; ++i) {
		result = doca_devinfo_rep_get_vuid(rep_list[i], actual_vuid, sizeof(actual_vuid));
		if (result != DOCA_SUCCESS || strncmp(actual_vuid, vuid, DOCA_DEVINFO_REP_VUID_SIZE) != 0)
			continue;

		result = doca_dev_rep_open(rep_list[i], rep_out);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to open rep '%s': %s", vuid, doca_error_get_descr(result));
		goto out;
	}

	DOCA_LOG_ERR("No emulated representor found with VUID '%s'", vuid);
	result = DOCA_ERROR_NOT_FOUND;

out:
	doca_devinfo_rep_destroy_list(rep_list);
	return result;
}

/* Builds a doca_mmap that the host driver's PRP/SGL addresses can be wrapped
 * into via doca_buf_inventory_buf_get_by_addr(). Stage 1 caches the mmap so
 * stages 3 and 4 can reuse it for Identify and Read DMA. */
static doca_error_t cache_host_mmap(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devemu_pci_dev *pci_dev;
	struct doca_devemu_pci_ep *ep;
	struct doca_dev *initiator_dev;
	doca_error_t result;

	pci_dev = doca_devemu_nvme_dev_as_pci_dev(app->nvme_dev);
	if (pci_dev == NULL) {
		DOCA_LOG_ERR("Failed to get pci_dev from nvme_dev");
		return DOCA_ERROR_BAD_STATE;
	}
	ep = doca_devemu_pci_dev_as_ep(pci_dev);
	if (ep == NULL) {
		DOCA_LOG_ERR("Failed to get pci_ep from pci_dev");
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_devemu_pci_ep_mmap_create(ep, &app->host_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create host mmap from pci_ep: %s", doca_error_get_descr(result));
		return result;
	}

	/* The initiator RDMAs values directly into host buffers, so its device needs the mapping too. */
	initiator_dev = memos_initiator_ref_app_kv_get_dev(app);

	result = doca_mmap_set_max_num_devices(app->host_mmap, initiator_dev != NULL ? 2 : 1);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_mmap_add_dev(app->host_mmap, app->dev);
	if (result != DOCA_SUCCESS)
		goto fail;
	if (initiator_dev != NULL) {
		result = doca_mmap_add_dev(app->host_mmap, initiator_dev);
		if (result != DOCA_SUCCESS)
			goto fail;
	}
	result = doca_mmap_set_permissions(app->host_mmap,
					   DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
						   DOCA_ACCESS_FLAG_RDMA_WRITE | DOCA_ACCESS_FLAG_PCI_RELAXED_ORDERING);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_mmap_set_memrange(app->host_mmap, NULL, UINT64_MAX);
	if (result != DOCA_SUCCESS)
		goto fail;
	/* Every IO worker acquires and releases bufs over this one mmap from its own thread. */
	result = doca_mmap_enable_thread_safety(app->host_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;
	result = doca_mmap_start(app->host_mmap);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	DOCA_LOG_ERR("Failed to configure host mmap: %s", doca_error_get_descr(result));
	(void)doca_mmap_destroy(app->host_mmap);
	app->host_mmap = NULL;
	return result;
}

/* Rebuild host_mmap so it embeds the crossing mkey currently cached on the ep. A Function Level
 * Reset between two enables rotates that firmware mkey; the lib refreshes ep.introspection_mkey on
 * the enabled 0->1 edge (which precedes the host's CC.EN 0->1 write), so recreating the mmap here
 * picks up the live key and admin data DMA (Identify PRP transfers) targets valid memory. Idempotent
 * (destroys any existing mmap first) and safe vs the startup caching: the admin DMA engine resolves
 * app->host_mmap per transfer and holds no cached mmap/mkey, and nothing else retains a reference
 * across the rebuild (host_mmap lives only in cache_host_mmap()/this helper and per-op DMA gets). */
static doca_error_t rebuild_host_mmap(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result;

	if (app->host_mmap != NULL) {
		result = doca_mmap_stop(app->host_mmap);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to stop stale host mmap on rebuild: %s", doca_error_get_descr(result));
		result = doca_mmap_destroy(app->host_mmap);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy stale host mmap on rebuild: %s", doca_error_get_descr(result));
		app->host_mmap = NULL;
	}

	return cache_host_mmap(app);
}

static const char *reg_offset_to_name(uint32_t offset)
{
	switch (offset) {
	case NVME_REG_CAP:
		return "CAP";
	case NVME_REG_VS:
		return "VS";
	case NVME_REG_INTMS:
		return "INTMS";
	case NVME_REG_INTMC:
		return "INTMC";
	case NVME_REG_CC:
		return "CC";
	case NVME_REG_CSTS:
		return "CSTS";
	case NVME_REG_AQA:
		return "AQA";
	case NVME_REG_ASQ:
		return "ASQ";
	case NVME_REG_ACQ:
		return "ACQ";
	default:
		return "?";
	}
}

/* Mirrors EN <-> RDY per the NVMe spec: when the host sets CC.EN we bring up the admin queues
 * (CQ, SQ and the io ctx that handles admin commands) and publish CSTS.RDY; when it clears CC.EN
 * we tear the admin queues down and clear CSTS.RDY. */
static void handle_cc_write(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devemu_nvme_dev *nvme_dev = app->nvme_dev;
	uint32_t cc;
	uint32_t csts;
	doca_error_t result;

	result = doca_devemu_nvme_dev_read_reg32(nvme_dev, NVME_REG_CC, &cc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read CC: %s", doca_error_get_descr(result));
		return;
	}
	result = doca_devemu_nvme_dev_read_reg32(nvme_dev, NVME_REG_CSTS, &csts);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read CSTS: %s", doca_error_get_descr(result));
		return;
	}

	const bool en = (cc & NVME_CC_EN_MASK) != 0;
	const uint8_t shn = (uint8_t)((cc & NVME_CC_SHN_MASK) >> NVME_CC_SHN_SHIFT);

	/* We attached while the host already had the controller enabled. Keep CSTS.CFS asserted and
	 * refuse bring-up until the host resets the controller (CC.EN 1->0); only then clear the fatal
	 * status so the subsequent clean CC.EN 0->1 enable is honoured. */
	if (app->awaiting_reset) {
		if (!en) {
			/* Clean reset observed (CC.EN 1->0): clear the fatal status and the gate. */
			csts &= ~(NVME_CSTS_CFS_MASK | NVME_CSTS_RDY_MASK);
			result = doca_devemu_nvme_dev_write_reg32(nvme_dev, NVME_REG_CSTS, csts);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to clear CSTS.CFS on reset: %s", doca_error_get_descr(result));
				return;
			}
			app->awaiting_reset = false;
			DOCA_LOG_INFO("Controller reset observed (CC.EN=0); CSTS.CFS cleared, ready for clean enable");
			return;
		}

		/* Host re-enabled directly (CC.EN=1) with freshly-programmed admin queues (AQA/ASQ/ACQ written
		 * in the same host init sequence) - e.g. SPDK, or a host whose CC.EN 1->0->1 reset coalesced into
		 * a single BAR0 diff so the 1->0 edge was never observed. There is no stale queue state to protect
		 * against: adopt the fresh programming. Clear the gate and the fatal status and fall through to the
		 * normal enable path, which brings up the admin queues and publishes CSTS.RDY. */
		DOCA_LOG_INFO("awaiting_reset: host re-enabled directly (CC.EN=1); adopting fresh host programming");
		app->awaiting_reset = false;
		csts &= ~NVME_CSTS_CFS_MASK;
	}

	/* Host-initiated shutdown (CC.SHN != 0, with CC.EN still 1): drain and tear down the queues,
	 * then report CSTS.SHST = complete (and clear RDY) so the host's shutdown poll finishes instead
	 * of timing out. This must run before the enable-edge gate below, which SHN would otherwise
	 * skip since CC.EN has not changed. */
	if (shn != 0) {
		if (app->admin_up) {
			/* Logged inside; the queue teardown below still disables whatever is left. */
			(void)nvme_dev_disable_and_wait(app);
			memos_initiator_ref_app_admin_tear_down(app);
			/* Atomic store: IO worker threads gate their busy-poll on admin_up (relaxed read). */
			__atomic_store_n(&app->admin_up, false, __ATOMIC_RELAXED);
			/* Tear down the IO datapath workers (and their DMA QPs) too: a reset/FLR cannot
			 * complete in firmware while any datapath QP is still bound to the function, so
			 * 'enabled' would never return to 1 (breaking MSI-X and eventually wedging the DMA
			 * QPs). Rebuilt fresh on the next enable. */
			memos_initiator_ref_app_workers_stop(app);
		}
		csts = (csts & ~(NVME_CSTS_RDY_MASK | NVME_CSTS_SHST_MASK)) | NVME_CSTS_SHST_COMPLETE;
		result = doca_devemu_nvme_dev_write_reg32(nvme_dev, NVME_REG_CSTS, csts);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to write CSTS.SHST on shutdown: %s", doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Controller shutdown processed (CC.SHN=%u -> CSTS.SHST=complete)", shn);
		return;
	}

	/* SHN cleared as part of a controller reset: drop any lingering SHST so the next clean enable
	 * starts from a normal shutdown status. */
	if ((csts & NVME_CSTS_SHST_MASK) != 0) {
		csts &= ~NVME_CSTS_SHST_MASK;
		result = doca_devemu_nvme_dev_write_reg32(nvme_dev, NVME_REG_CSTS, csts);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to clear CSTS.SHST on reset: %s", doca_error_get_descr(result));
			return;
		}
	}

	/* Gate on our own admin_up state, not CSTS.RDY: an FLR clears CC and CSTS together, so a
	 * CSTS.RDY-based check would treat the FLR disable edge as a no-op and skip queue teardown,
	 * leaving the queues bound to now-stale host ASQ/ACQ addresses (DMAR PTE Read faults). */
	if (en == app->admin_up)
		return;

	if (en) {
		/* Rebuild host_mmap against the (possibly FLR-rotated) crossing mkey before bring-up. The
		 * lib refreshed ep.introspection_mkey on the enabled 0->1 edge, which the firmware raises
		 * before the host can write CC.EN=1, so by now the ep carries the live key. The admin DMA
		 * engine resolves app->host_mmap per transfer (no cached mmap/mkey), so rebuilding the mmap
		 * alone re-points admin data DMA; no DMA-engine re-init is required. */
		result = rebuild_host_mmap(app);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to rebuild host mmap on enable, not signalling CSTS.RDY: %s",
				     doca_error_get_descr(result));
			return;
		}

		/* (Re)create the IO datapath workers and their DMA QPs against the now re-enabled
		 * function. A prior controller-disable (CC.EN 1->0 / shutdown / FLR) tore them down so the
		 * firmware could complete the reset and bring 'enabled' back to 1; rebuild them fresh here
		 * before admin bring-up. Guarded on app->workers so this is a no-op on the first enable
		 * after startup, where workers_start() already ran. */
		if (app->workers == NULL) {
			result = memos_initiator_ref_app_workers_start(app);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to (re)start IO workers on enable, not signalling CSTS.RDY: %s",
					     doca_error_get_descr(result));
				return;
			}
		}

		result = nvme_dev_enable(app);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("NVMe dev enable failed, not signalling CSTS.RDY: %s",
				     doca_error_get_descr(result));
			return;
		}

		/* Host has programmed AQA/ASQ/ACQ already; bring up the admin queues before
		 * signalling readiness so the host driver sees a meaningful RDY. */
		result = memos_initiator_ref_app_admin_bring_up(app);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Admin queue bring-up failed, not signalling CSTS.RDY: %s",
				     doca_error_get_descr(result));
			/* admin_up stays false, so a later CC.EN=0 or shutdown skips its disable: undo the
			 * enable here. Logged inside. */
			(void)nvme_dev_disable_and_wait(app);
			return;
		}
		__atomic_store_n(&app->admin_up, true, __ATOMIC_RELAXED);
		csts |= NVME_CSTS_RDY_MASK;
	} else {
		/* Logged inside; the queue teardown below still disables whatever is left. */
		(void)nvme_dev_disable_and_wait(app);
		memos_initiator_ref_app_admin_tear_down(app);
		__atomic_store_n(&app->admin_up, false, __ATOMIC_RELAXED);
		/* See the shutdown path above: drop the IO datapath workers (and DMA QPs) so the
		 * function's reset/FLR can complete and 'enabled' can return to 1; rebuilt on next enable. */
		memos_initiator_ref_app_workers_stop(app);
		csts &= ~NVME_CSTS_RDY_MASK;
	}

	result = doca_devemu_nvme_dev_write_reg32(nvme_dev, NVME_REG_CSTS, csts);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to write CSTS: %s", doca_error_get_descr(result));
		return;
	}
	DOCA_LOG_INFO("Controller %s (CC.EN=%u -> CSTS.RDY=%u)", en ? "enabled" : "disabled", en, en);
}

/* Put the host-programmable controller registers back to their power-on (zeroed) values.
 *
 * BAR0 lives in the firmware emulation object, so it outlives a controller reset and this process
 * alike, while the library's reg-change transport reports a host write by diffing BAR0 against a
 * shadow that starts at zero. Leftovers therefore break both directions: they are replayed as
 * fresh host writes, and - worse - they swallow a real one, because a host that writes exactly
 * what is already there produces no diff and so no event at all. */
static doca_error_t reset_host_programmed_regs(struct memos_initiator_ref_app_resources *app)
{
	static const uint32_t reg64_reset[] = {NVME_REG_ASQ, NVME_REG_ACQ};
	static const uint32_t reg32_reset[] = {NVME_REG_CSTS, NVME_REG_AQA, NVME_REG_CC};
	doca_error_t reset = DOCA_SUCCESS;
	doca_error_t result;
	size_t i;

	/* Every register is attempted even after one fails, because a partly cleared BAR0 is worse than
	 * either extreme, and the first failure is what the caller hears about: a controller presented
	 * to the host with any of this state left over is the condition this clears. */
	for (i = 0; i < sizeof(reg64_reset) / sizeof(reg64_reset[0]); ++i) {
		result = doca_devemu_nvme_dev_write_reg64(app->nvme_dev, reg64_reset[i], 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to reset reg 0x%02x: %s", reg64_reset[i], doca_error_get_descr(result));
			DOCA_ERROR_PROPAGATE(reset, result);
		}
	}
	for (i = 0; i < sizeof(reg32_reset) / sizeof(reg32_reset[0]); ++i) {
		result = doca_devemu_nvme_dev_write_reg32(app->nvme_dev, reg32_reset[i], 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to reset reg 0x%02x: %s", reg32_reset[i], doca_error_get_descr(result));
			DOCA_ERROR_PROPAGATE(reset, result);
		}
	}

	return reset;
}

/* Whether a host driver can already be attached to the function we are taking over: a static
 * function is powered on by firmware at boot, and a hotplugged one is still host-visible if a
 * previous run left it powered on - it is only powered off on an orderly exit. */
static doca_error_t function_is_host_visible(struct memos_initiator_ref_app_resources *app, bool *visible)
{
	struct doca_devemu_pci_dev *pci_dev;
	enum doca_devemu_pci_hotplug_state state;
	doca_error_t result;

	if (app->rep_is_static) {
		*visible = true;
		return DOCA_SUCCESS;
	}

	pci_dev = doca_devemu_nvme_dev_as_pci_dev(app->nvme_dev);
	if (pci_dev == NULL) {
		DOCA_LOG_ERR("Failed to get pci_dev from nvme_dev for the hotplug state query");
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get hotplug state: %s", doca_error_get_descr(result));
		return result;
	}

	*visible = (state == DOCA_DEVEMU_PCI_HP_STATE_POWER_ON);
	return DOCA_SUCCESS;
}

/* Detect a host driver that already enabled the controller before this (possibly restarted) app
 * attached. Our fresh state has no knowledge of the host's programmed AQA/ASQ/ACQ or IO queues, so
 * bringing queues up now would DMA from/to addresses owned by a different controller incarnation.
 * Per NVMe we assert CSTS.CFS (and clear CSTS.RDY) so the host performs a controller reset; the
 * awaiting_reset gate in handle_cc_write then refuses bring-up until we observe CC.EN 1->0. A
 * disabled controller only has a stale CSTS cleared. */
static doca_error_t flag_if_host_active(struct memos_initiator_ref_app_resources *app)
{
	uint32_t cc;
	uint32_t csts;
	doca_error_t result;

	result = doca_devemu_nvme_dev_read_reg32(app->nvme_dev, NVME_REG_CC, &cc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read CC at startup: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_nvme_dev_read_reg32(app->nvme_dev, NVME_REG_CSTS, &csts);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read CSTS at startup: %s", doca_error_get_descr(result));
		return result;
	}

	if ((cc & NVME_CC_EN_MASK) == 0) {
		if (csts == 0)
			return DOCA_SUCCESS;

		/* The host disabled the controller while no app was serving it, so CSTS was never
		 * cleared. Its reset writes CC.EN=0 again, which matches BAR0 and raises no event,
		 * and then polls for RDY=0 until it times out. */
		DOCA_LOG_WARN("Stale CSTS=0x%08x with CC.EN=0 at attach; clearing CSTS", csts);
		result = doca_devemu_nvme_dev_write_reg32(app->nvme_dev, NVME_REG_CSTS, 0);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to clear stale CSTS at startup: %s", doca_error_get_descr(result));
		return result;
	}

	/* CC.EN=1 but the previous incarnation already completed a shutdown (CSTS.SHST=complete): the
	 * controller is quiesced and not actively running queues, so there is no live host queue state to
	 * collide with. Proceed with normal startup instead of forcing the CFS/await-reset path. */
	if ((csts & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_COMPLETE) {
		DOCA_LOG_INFO(
			"Host controller enabled (CC.EN=1) but shutdown-complete (CSTS.SHST=complete) at attach; proceeding with normal startup");
		return DOCA_SUCCESS;
	}

	DOCA_LOG_WARN(
		"Host controller already enabled (CC.EN=1) at attach; asserting CSTS.CFS and waiting for controller reset");

	/* The library's reg-change translator diffs host writes against an internal BAR0 shadow that
	 * starts at 0, but the live CC already reads EN=1 (written before we attached). Write CC back
	 * to its current value - a no-op for the host - so the shadow matches and the host's later
	 * reset write (CC.EN 1->0) is detected and delivered to handle_cc_write. */
	result = doca_devemu_nvme_dev_write_reg32(app->nvme_dev, NVME_REG_CC, cc);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to sync CC shadow at startup: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_nvme_dev_read_reg32(app->nvme_dev, NVME_REG_CSTS, &csts);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to read CSTS at startup: %s", doca_error_get_descr(result));
		return result;
	}
	csts = (csts & ~NVME_CSTS_RDY_MASK) | NVME_CSTS_CFS_MASK;
	result = doca_devemu_nvme_dev_write_reg32(app->nvme_dev, NVME_REG_CSTS, csts);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to assert CSTS.CFS at startup: %s", doca_error_get_descr(result));
		return result;
	}

	app->awaiting_reset = true;
	return DOCA_SUCCESS;
}

static void reg_change_cb(struct doca_devemu_nvme_dev *nvme_dev, uint32_t offset, union doca_data user_data)
{
	struct memos_initiator_ref_app_resources *app = user_data.ptr;

	uint64_t value64;
	uint32_t value32;
	doca_error_t result;

	switch (offset) {
	case NVME_REG_CAP:
	case NVME_REG_ASQ:
	case NVME_REG_ACQ:
		result = doca_devemu_nvme_dev_read_reg64(nvme_dev, offset, &value64);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to read %s @0x%02x: %s",
				     reg_offset_to_name(offset),
				     offset,
				     doca_error_get_descr(result));
		else
			DOCA_LOG_INFO("BAR0 write: %s @0x%02x = 0x%016" PRIx64,
				      reg_offset_to_name(offset),
				      offset,
				      value64);
		break;
	default:
		result = doca_devemu_nvme_dev_read_reg32(nvme_dev, offset, &value32);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to read %s @0x%02x: %s",
				     reg_offset_to_name(offset),
				     offset,
				     doca_error_get_descr(result));
		else
			DOCA_LOG_INFO("BAR0 write: %s @0x%02x = 0x%08x", reg_offset_to_name(offset), offset, value32);
		break;
	}

	if (offset == NVME_REG_CC)
		handle_cc_write(app);
}

/* FLR notification from the library (emulation object 'enabled' 1->0). An FLR is host-driven and
 * asynchronous and the firmware has torn the emulation object down, so we cannot recover inline: the
 * handler runs on the control PE and recovery needs to stop and re-start the device ctx. Just flag it;
 * the main control loop drives memos_initiator_ref_app_recover_flr() once the handler returns. */
static void flr_cb(struct doca_devemu_pci_dev *pci_dev, union doca_data user_data)
{
	struct memos_initiator_ref_app_resources *app = user_data.ptr;

	(void)pci_dev;
	DOCA_LOG_WARN("FLR detected: scheduling controller stop/start recovery");
	app->flr_pending = true;
}

/* Returns elapsed microseconds from `start` to now using CLOCK_MONOTONIC, so PE-polling timeouts
 * are wall-clock based and not coupled to doca_pe_progress() return values or nanosleep slop. */
static size_t elapsed_us_since(const struct timespec *start)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (size_t)((now.tv_sec - start->tv_sec) * 1000000LL + (now.tv_nsec - start->tv_nsec) / 1000LL);
}

/* Stop a ctx and drive the PE until it reaches IDLE, so the subsequent destroy can succeed.
 * doca_ctx_stop() may return DOCA_ERROR_IN_PROGRESS and complete asynchronously via PE events;
 * destroy APIs (nvme_dev / pci_dev) refuse anything other than IDLE. */
static doca_error_t stop_ctx_and_wait_idle(struct memos_initiator_ref_app_resources *app,
					   struct doca_ctx *ctx,
					   size_t timeout_us)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_PROGRESS_SLEEP_NS};
	struct timespec start;
	enum doca_ctx_states state;
	doca_error_t result;

	result = doca_ctx_stop(ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE) {
		DOCA_LOG_ERR("Failed to stop ctx: %s", doca_error_get_descr(result));
		return result;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		result = doca_ctx_get_state(ctx, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query ctx state: %s", doca_error_get_descr(result));
			return result;
		}
		if (state == DOCA_CTX_STATE_IDLE)
			return DOCA_SUCCESS;
		if (elapsed_us_since(&start) >= timeout_us) {
			DOCA_LOG_ERR("Timed out waiting for ctx to reach IDLE (state=%d)", (int)state);
			return DOCA_ERROR_TIME_OUT;
		}
		/* The per-core IO workers were already stopped before nvme_dev teardown; the admin io ctx
		 * and DMA ride admin_pe, so pumping the two main PEs is enough to drain to IDLE. */
		if (memos_initiator_ref_app_progress_all(app) == 0) {
			struct timespec ts = sleep_ts;
			(void)nanosleep(&ts, NULL);
		}
	}
}

/* Wait for the NVMe device to reach DISABLED, driving the PEs that complete its queue disables. */
static doca_error_t nvme_dev_wait_disabled(struct memos_initiator_ref_app_resources *app)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_PROGRESS_SLEEP_NS};
	enum doca_devemu_nvme_dev_states state;
	struct timespec start;
	doca_error_t result;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		result = doca_devemu_nvme_dev_get_state(app->nvme_dev, &state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query NVMe dev state: %s", doca_error_get_descr(result));
			return result;
		}
		if (state == DOCA_DEVEMU_NVME_DEV_STATE_DISABLED)
			return DOCA_SUCCESS;
		if (state != DOCA_DEVEMU_NVME_DEV_STATE_DISABLING) {
			DOCA_LOG_ERR("NVMe dev reached state %d instead of DISABLED", (int)state);
			return DOCA_ERROR_BAD_STATE;
		}
		if (elapsed_us_since(&start) >= MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US) {
			DOCA_LOG_ERR("NVMe dev did not finish disabling within %u s",
				     (unsigned int)(MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US / 1000000));
			return DOCA_ERROR_TIME_OUT;
		}
		if (memos_initiator_ref_app_progress_all(app) == 0) {
			struct timespec ts = sleep_ts;
			(void)nanosleep(&ts, NULL);
		}
	}
}

/* The library refuses to enable a queue unless the device is ENABLED, and the device starts out
 * DISABLED. Enable it on the host's CC.EN 0->1 edge, before the admin queues are brought up. */
static doca_error_t nvme_dev_enable(struct memos_initiator_ref_app_resources *app)
{
	enum doca_devemu_nvme_dev_states state;
	doca_error_t result;

	result = doca_devemu_nvme_dev_get_state(app->nvme_dev, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query NVMe dev state: %s", doca_error_get_descr(result));
		return result;
	}
	if (state == DOCA_DEVEMU_NVME_DEV_STATE_ENABLED)
		return DOCA_SUCCESS;

	result = nvme_dev_wait_disabled(app);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_devemu_nvme_dev_enable(app->nvme_dev);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to enable NVMe dev: %s", doca_error_get_descr(result));
	return result;
}

/* Disable the NVMe device and wait for DISABLED. The library disables every enabled queue itself, so
 * this runs before the queues are torn down, which then finds them already disabled. A held
 * Async-Event-Request keeps the admin SQ from draining, so it is completed between the disable
 * request and the wait, as the DOCA Memos DPU service does. */
static doca_error_t nvme_dev_disable_and_wait(struct memos_initiator_ref_app_resources *app)
{
	enum doca_devemu_nvme_dev_states state;
	doca_error_t result;

	if (app->nvme_dev == NULL)
		return DOCA_SUCCESS;

	result = doca_devemu_nvme_dev_get_state(app->nvme_dev, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query NVMe dev state: %s", doca_error_get_descr(result));
		return result;
	}
	if (state == DOCA_DEVEMU_NVME_DEV_STATE_ENABLED) {
		result = doca_devemu_nvme_dev_disable(app->nvme_dev);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
			DOCA_LOG_ERR("Failed to disable NVMe dev: %s", doca_error_get_descr(result));
			return result;
		}
	}

	if (app->admin_io != NULL)
		memos_initiator_ref_app_admin_complete_held_aers(app);

	return nvme_dev_wait_disabled(app);
}

/* Tear down every datapath resource the app created on top of the (running) nvme_dev: admin queues,
 * IO datapath workers, the admin DMA engine and the host mmap. Ordered so nothing is left in flight or
 * bound to the emulation object: admin queues drain first (releasing their DB CQ mappings while the
 * object is still alive), then the IO workers, then the DMA engine (it resolves against host_mmap),
 * then host_mmap itself. Every step is idempotent, so this is safe whether or not the controller was
 * enabled. Shared by teardown() and FLR recovery. */
static doca_error_t quiesce_datapath(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t quiesced = DOCA_SUCCESS;
	doca_error_t result;

	/* Logged inside; the queue teardown below still disables whatever is left. */
	(void)nvme_dev_disable_and_wait(app);
	memos_initiator_ref_app_admin_tear_down(app);
	/* IO worker threads gate their busy-poll on admin_up (relaxed read). */
	__atomic_store_n(&app->admin_up, false, __ATOMIC_RELAXED);
	memos_initiator_ref_app_workers_stop(app);

	/* The DMA engine is the one step here that can decline to finish: it keeps itself alive when a
	 * task may still complete into it. Carry that out so a caller that means to rebuild the
	 * datapath does not stand a second engine up beside the first. */
	quiesced = memos_initiator_ref_app_dma_teardown(app);

	if (app->host_mmap != NULL) {
		result = doca_mmap_stop(app->host_mmap);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to stop host mmap: %s", doca_error_get_descr(result));
		result = doca_mmap_destroy(app->host_mmap);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy host mmap: %s", doca_error_get_descr(result));
		app->host_mmap = NULL;
	}

	return quiesced;
}

static void teardown(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result;

	/* Remove the device from the host first so it stops issuing new commands, then drain. Idempotent
	 * (no-op if we never powered on); leaves the rep in firmware for a later run to power on again.
	 * Static functions are firmware-owned and not hotplug devices - we never powered them on, so we
	 * must not power them off (firmware keeps them visible to the host across our lifetime). */
	if (!app->rep_is_static)
		(void)power_off_function(app);

	/* Drain and release the datapath (admin queues, IO workers, DMA, host mmap) while the dev ctx is
	 * still running, so in-flight requests flush and DB CQ mappings are released before ctx stop.
	 * On the way out there is nothing left to protect, so a retained engine is reported and the
	 * teardown continues. */
	(void)quiesce_datapath(app);

	/* Not in quiesce_datapath(): that runs on FLR recovery too, and a host that reset its
	 * controller has not asked for its stored values to be discarded. The store outlives every
	 * reset and is released only when the app exits, once no worker can still reach it. */
	memos_initiator_ref_app_kv_teardown(app);

	if (app->nvme_dev != NULL) {
		struct doca_ctx *ctx = doca_devemu_nvme_dev_as_ctx(app->nvme_dev);

		if (ctx != NULL)
			(void)stop_ctx_and_wait_idle(app, ctx, MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US);
		result = doca_devemu_nvme_dev_destroy(app->nvme_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMe device: %s", doca_error_get_descr(result));
		app->nvme_dev = NULL;
	}
	if (app->rep != NULL) {
		result = doca_dev_rep_close(app->rep);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close emulated rep: %s", doca_error_get_descr(result));
		app->rep = NULL;
	}
	/* The app owns the pci_type; release it before tearing the library down. */
	destroy_nvme_pci_type(app);
	result = doca_devemu_nvme_teardown();
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to teardown NVMe library: %s", doca_error_get_descr(result));
	if (app->dev != NULL) {
		result = doca_devemu_nvme_rm_dev(app->dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to remove NVMe dev registration: %s", doca_error_get_descr(result));
		result = doca_dev_close(app->dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA dev: %s", doca_error_get_descr(result));
		app->dev = NULL;
	}
	/* PEs must outlive every ctx: the IDLE transitions above drive events through them. The admin
	 * io ctx and admin DMA rode admin_pe (torn down above); the per-core worker PEs were destroyed
	 * by memos_initiator_ref_app_workers_stop(). So it is safe to destroy admin_pe now. */
	if (app->admin_pe != NULL) {
		result = doca_pe_destroy(app->admin_pe);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy admin progress engine: %s", doca_error_get_descr(result));
		app->admin_pe = NULL;
	}
	if (app->pe != NULL) {
		result = doca_pe_destroy(app->pe);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_descr(result));
		app->pe = NULL;
	}
}

static const char *pci_func_type_label(enum doca_pci_func_type t)
{
	switch (t) {
	case DOCA_PCI_FUNC_TYPE_PF:
		return "PF";
	case DOCA_PCI_FUNC_TYPE_VF:
		return "VF";
	case DOCA_PCI_FUNC_TYPE_SF:
		return "SF";
	default:
		return "?";
	}
}

/* Best-effort emulation-type tag inferred from the VUID. The firmware tags
 * emulated VUIDs with a per-opmod suffix after the serial:
 *   NVMES  -> NVMe       (e.g. 'MT2306XZ007CNVMES0D0F2')
 *   VBLKS  -> virtio_blk
 *   VNETS  -> virtio_net
 *   VFSS   -> virtio_fs
 *   TLPS   -> tlp
 *   Gxx    -> generic-PCI (e.g. 'MT2306XZ007CGxx1D0F0' with 'xx' = ES)
 * DOCA does not yet expose the emulation opmod publicly; when it does,
 * replace this with the proper accessor. */
static const char *emulation_type_from_vuid(const char *vuid)
{
	if (vuid == NULL)
		return "?";
	if (strstr(vuid, "NVMES") != NULL)
		return "nvme";
	if (strstr(vuid, "VBLKS") != NULL)
		return "virtio_blk";
	if (strstr(vuid, "VNETS") != NULL)
		return "virtio_net";
	if (strstr(vuid, "VFSS") != NULL)
		return "virtio_fs";
	if (strstr(vuid, "TLPS") != NULL)
		return "tlp";
	/* "G" "ES" split keeps codespell/typos quiet on the literal generic-PCI tag. */
	if (strstr(vuid,
		   "G"
		   "ES") != NULL)
		return "generic";
	return "unknown";
}

/* BAR0 map for the 'generic' kind. A generic PCI type
 * needs every region spelled out, and firmware indexes generic pci_types by a (name, content)
 * fingerprint: it rejects a create with the same name but different content. So this kind carries its
 * own type name and these offsets have to stay as they are - sharing one name across the two kinds
 * would make whichever ran second fail, and would let the per-pci_type rep filter match functions
 * created under the other layout. The distinct name is also what --list-emulated-functions reports,
 * which is the only way to tell which layout a function was created under.
 *
 * BAR0 is sized to exactly cover the last region (0x3000 + 4 KB). The PCI identity is not repeated
 * here: both kinds use the same MEMOS_INITIATOR_REF_APP_PCI_* values, so a host driver sees the same controller
 * either way. */
#define MEMOS_INITIATOR_REF_APP_PCI_TYPE_NAME_GENERIC "DOCA_DEVEMU_NVME_GEN"
#define MEMOS_INITIATOR_REF_APP_BAR0_LOG_SIZE 0xe /* 16 KB */
#define MEMOS_INITIATOR_REF_APP_BAR1_LOG_SIZE 0x0 /* unused */
#define MEMOS_INITIATOR_REF_APP_BAR0_STATEFUL_OFFSET 0x0
#define MEMOS_INITIATOR_REF_APP_BAR0_STATEFUL_SIZE 64
#define MEMOS_INITIATOR_REF_APP_BAR0_DB_OFFSET 0x1000
#define MEMOS_INITIATOR_REF_APP_BAR0_DB_SIZE 4096
#define MEMOS_INITIATOR_REF_APP_DB_LOG_SIZE 0x2	  /* 4 B doorbell record */
#define MEMOS_INITIATOR_REF_APP_DB_LOG_STRIDE 0x2 /* CAP.DSTRD == 0 */
#define MEMOS_INITIATOR_REF_APP_BAR0_MSIX_TABLE_OFFSET 0x2000
#define MEMOS_INITIATOR_REF_APP_BAR0_MSIX_TABLE_SIZE 4096
#define MEMOS_INITIATOR_REF_APP_BAR0_MSIX_PBA_OFFSET 0x3000
#define MEMOS_INITIATOR_REF_APP_BAR0_MSIX_PBA_SIZE 4096

/* Spell out the NVMe BAR0 map on a raw generic pci_type. Only the 'generic' kind needs this; the
 * canned kind has firmware supply the same geometry and rejects these setters. */
static doca_error_t set_custom_nvme_bar_layout(struct doca_devemu_pci_type *pci_type)
{
	doca_error_t result;

	result = doca_devemu_pci_type_set_memory_bar_conf(pci_type,
							  0,
							  MEMOS_INITIATOR_REF_APP_BAR0_LOG_SIZE,
							  DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT,
							  0);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_devemu_pci_type_set_memory_bar_conf(pci_type,
							  1,
							  MEMOS_INITIATOR_REF_APP_BAR1_LOG_SIZE,
							  DOCA_DEVEMU_PCI_BAR_MEM_TYPE_64_BIT,
							  0);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_devemu_pci_type_set_bar_stateful_region_conf(pci_type,
								   0,
								   MEMOS_INITIATOR_REF_APP_BAR0_STATEFUL_OFFSET,
								   MEMOS_INITIATOR_REF_APP_BAR0_STATEFUL_SIZE);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_devemu_pci_type_set_bar_db_region_by_offset_conf(pci_type,
								       0,
								       MEMOS_INITIATOR_REF_APP_BAR0_DB_OFFSET,
								       MEMOS_INITIATOR_REF_APP_BAR0_DB_SIZE,
								       MEMOS_INITIATOR_REF_APP_DB_LOG_SIZE,
								       MEMOS_INITIATOR_REF_APP_DB_LOG_STRIDE);
	if (result != DOCA_SUCCESS)
		return result;
	result = doca_devemu_pci_type_set_bar_msix_table_region_conf(pci_type,
								     0,
								     MEMOS_INITIATOR_REF_APP_BAR0_MSIX_TABLE_OFFSET,
								     MEMOS_INITIATOR_REF_APP_BAR0_MSIX_TABLE_SIZE);
	if (result != DOCA_SUCCESS)
		return result;

	return doca_devemu_pci_type_set_bar_msix_pba_region_conf(pci_type,
								 0,
								 MEMOS_INITIATOR_REF_APP_BAR0_MSIX_PBA_OFFSET,
								 MEMOS_INITIATOR_REF_APP_BAR0_MSIX_PBA_SIZE);
}

/* Build the NVMe pci_type for the kind --pci-type selected. The two generic-emulation kinds are owned
 * by the app and carry the same PCI identity, MSI-X and doorbell counts, differing only in where the
 * BAR0 map comes from - firmware supplies it for 'generic_nvme', the app spells it out for 'generic'.
 * The native kind is not built here at all: the library keeps one type per device and hands it out. */
static doca_error_t create_nvme_pci_type(struct memos_initiator_ref_app_resources *app)
{
	const bool custom_layout = (app->cfg.pci_type_kind == MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_GENERIC);
	const char *type_name = custom_layout ? MEMOS_INITIATOR_REF_APP_PCI_TYPE_NAME_GENERIC :
						MEMOS_INITIATOR_REF_APP_PCI_TYPE_NAME;
	doca_error_t result;

	/* The native type is not the app's to build: firmware places BAR0 and the PCI identity, and the
	 * library shares one started type between every user of the device. Take it as it comes - none
	 * of the configuration below applies, and every setter would fail on a started type anyway. */
	if (app->cfg.pci_type_kind == MEMOS_INITIATOR_REF_APP_PCI_TYPE_KIND_NATIVE) {
		DOCA_LOG_INFO("Acquiring the library-owned native NVMe pci_type");
		result = doca_devemu_nvme_find_default_nvme_pci_type_by_dev(app->dev, &app->pci_type);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to acquire the native NVMe pci_type: %s", doca_error_get_descr(result));
		return result;
	}

	if (custom_layout) {
		DOCA_LOG_INFO("Creating generic pci_type '%s' with an application-supplied NVMe BAR layout", type_name);
		result = doca_devemu_pci_type_create(type_name, &app->pci_type);
	} else {
		DOCA_LOG_INFO("Creating NVMe pci_type '%s' with the firmware-canned NVMe BAR layout", type_name);
		result = doca_devemu_nvme_pci_type_create(type_name, &app->pci_type);
	}
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMe pci_type: %s", doca_error_get_descr(result));
		return result;
	}

	if (custom_layout) {
		result = set_custom_nvme_bar_layout(app->pci_type);
		if (result != DOCA_SUCCESS)
			goto destroy;
	}

	result = doca_devemu_pci_type_set_dev(app->pci_type, app->dev);
	if (result != DOCA_SUCCESS)
		goto destroy;

	result = doca_devemu_pci_type_set_vendor_id(app->pci_type, MEMOS_INITIATOR_REF_APP_PCI_VENDOR_ID);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_devemu_pci_type_set_device_id(app->pci_type, MEMOS_INITIATOR_REF_APP_PCI_DEVICE_ID);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_devemu_pci_type_set_subsystem_vendor_id(app->pci_type,
							      MEMOS_INITIATOR_REF_APP_PCI_SUBSYSTEM_VENDOR_ID);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_devemu_pci_type_set_subsystem_id(app->pci_type, MEMOS_INITIATOR_REF_APP_PCI_SUBSYSTEM_ID);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_devemu_pci_type_set_revision_id(app->pci_type, MEMOS_INITIATOR_REF_APP_PCI_REVISION_ID);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_devemu_pci_type_set_class_code(app->pci_type, MEMOS_INITIATOR_REF_APP_PCI_CLASS_CODE);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_devemu_pci_type_set_num_msix(app->pci_type, MEMOS_INITIATOR_REF_APP_PCI_NUM_MSIX);
	if (result != DOCA_SUCCESS)
		goto destroy;
	result = doca_devemu_pci_type_set_num_db(app->pci_type, MEMOS_INITIATOR_REF_APP_PCI_NUM_DB);
	if (result != DOCA_SUCCESS)
		goto destroy;

	result = doca_devemu_pci_type_start(app->pci_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start NVMe pci_type: %s", doca_error_get_descr(result));
		goto destroy;
	}

	return DOCA_SUCCESS;

destroy:
	DOCA_LOG_ERR("Failed to configure NVMe pci_type: %s", doca_error_get_descr(result));
	(void)doca_devemu_pci_type_destroy(app->pci_type);
	app->pci_type = NULL;

	return result;
}

/* Stop and destroy the app-owned pci_type. Safe to call when app->pci_type is NULL, and a no-op for
 * the default NVMe type, which the library owns and shares between every user of the device. */
static void destroy_nvme_pci_type(struct memos_initiator_ref_app_resources *app)
{
	uint8_t is_default = 0;
	doca_error_t result;

	if (app->pci_type == NULL)
		return;

	if (doca_devemu_nvme_is_default_nvme_pci_type(app->pci_type, &is_default) == DOCA_SUCCESS && is_default) {
		app->pci_type = NULL;
		return;
	}

	result = doca_devemu_pci_type_stop(app->pci_type);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
		DOCA_LOG_ERR("Failed to stop NVMe pci_type: %s", doca_error_get_descr(result));
	result = doca_devemu_pci_type_destroy(app->pci_type);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to destroy NVMe pci_type: %s", doca_error_get_descr(result));
	app->pci_type = NULL;
}

/* Install the NVMe BAR0 CAP/VS reset defaults on the (started, rep-less) pci_type using only public
 * DOCA PCI APIs, so firmware restores them on power-on/FLR and a host reads valid capabilities on
 * first exposure. The firmware copies exactly region_size bytes and rejects a smaller request, so we
 * query the BAR0 stateful-region geometry and pass a zero-padded buffer of that size with CAP/VS at
 * the front. Must run while the type has no rep - firmware rejects type-level default modification
 * once a rep exists. Non-fatal: the controller is still host-usable via the live CAP/VS seed. */
static doca_error_t install_pci_type_bar_defaults(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devemu_pci_type *pci_type = app->pci_type;
	struct doca_devemu_pci_stateful_region_info **regions = NULL;
	uint8_t *defaults = NULL;
	uint8_t is_default = 0;
	uint32_t nb_regions = 0;
	uint64_t region_size = 0;
	/* Advertise the device-honored max I/O queue depth via CAP.MQES (zero-based = entries - 1). */
	uint64_t cap = MEMOS_INITIATOR_REF_APP_CAP_FROM_MQES(app->max_io_queue_size - 1);
	uint32_t vs = MEMOS_INITIATOR_REF_APP_DEFAULT_VS;
	bool found = false;
	doca_error_t result;

	/* The default NVMe type exposes no configurable BAR regions - firmware supplies BAR0 and its
	 * reset defaults for the functions it owns - so there is nothing to install. */
	if (doca_devemu_nvme_is_default_nvme_pci_type(pci_type, &is_default) == DOCA_SUCCESS && is_default)
		return DOCA_SUCCESS;

	result = doca_devemu_pci_type_create_stateful_region_info_list(pci_type, &regions, &nb_regions);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t i = 0; i < nb_regions; ++i) {
		uint8_t bar_id = 0xFF;
		uint64_t start_addr = 0;

		if (doca_devemu_pci_stateful_region_info_get_bar_id(regions[i], &bar_id) != DOCA_SUCCESS ||
		    doca_devemu_pci_stateful_region_info_get_start_addr(regions[i], &start_addr) != DOCA_SUCCESS)
			continue;
		if (bar_id != 0 || start_addr != 0)
			continue;
		if (doca_devemu_pci_stateful_region_info_get_size(regions[i], &region_size) != DOCA_SUCCESS)
			continue;
		found = true;
		break;
	}

	if (!found || region_size < NVME_REG_VS + sizeof(vs)) {
		result = DOCA_ERROR_NOT_FOUND;
		goto destroy_list;
	}

	defaults = calloc(1, region_size);
	if (defaults == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_list;
	}

	memcpy(defaults + NVME_REG_CAP, &cap, sizeof(cap));
	memcpy(defaults + NVME_REG_VS, &vs, sizeof(vs));

	result = doca_devemu_pci_type_modify_bar_stateful_region_default_values(pci_type, 0, 0, defaults, region_size);

	free(defaults);

destroy_list:
	(void)doca_devemu_pci_type_destroy_stateful_region_info_list(regions);
	return result;
}

/* Write the CAP/VS reset defaults into the controller's BAR0 once the device is RUNNING, so the host
 * reads valid capabilities (notably CAP.TO != 0) on first access. The public write helpers also
 * mirror the values into the library's diff shadow, so the first host write does not falsely report a
 * CAP/VS change. This is the authoritative path for the firmware-canned NVMe BAR layout, where the
 * type-level default install does not apply. */
static doca_error_t seed_bar_cap_vs(struct memos_initiator_ref_app_resources *app)
{
	/* Advertise the device-honored max I/O queue depth via CAP.MQES (zero-based = entries - 1). */
	uint64_t cap = MEMOS_INITIATOR_REF_APP_CAP_FROM_MQES(app->max_io_queue_size - 1);
	doca_error_t result;

	result = doca_devemu_nvme_dev_write_reg64(app->nvme_dev, NVME_REG_CAP, cap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to seed BAR0 CAP: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_devemu_nvme_dev_write_reg32(app->nvme_dev, NVME_REG_VS, MEMOS_INITIATOR_REF_APP_DEFAULT_VS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to seed BAR0 VS: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Seeded BAR0 CAP=0x%016" PRIx64 " VS=0x%08x", cap, (unsigned)MEMOS_INITIATOR_REF_APP_DEFAULT_VS);
	return DOCA_SUCCESS;
}

/* Opens the emulation manager, registers it with the NVMe library, finalises the library, then
 * creates / configures / starts the app-owned NVMe pci_type. Shared by memos_initiator_ref_app_run, _hotplug and
 * _hotunplug so they all use the same pci_type identity (name, BAR layout, defaults). Unlike the
 * library-owned model, the application owns the pci_type lifetime here (per the public API) and must
 * release it via destroy_nvme_pci_type() during teardown. */
static doca_error_t prepare_pci_type_and_dev(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result;

	DOCA_LOG_INFO("Opening emulation manager device '%s'", app->cfg.pci_address);
	result = open_emulation_manager_dev(app->cfg.pci_address, &app->dev);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("Adding device to NVMe library");
	result = doca_devemu_nvme_add_dev(app->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add device to NVMe library: %s", doca_error_get_descr(result));
		return result;
	}

	/* Request a per-request user-data buffer from the library; must be set before init. */
	result = doca_devemu_nvme_set_req_user_data_size(MEMOS_INITIATOR_REF_APP_REQ_USER_DATA_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set req user data size: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Initializing NVMe library");
	result = doca_devemu_nvme_init();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init NVMe library: %s", doca_error_get_descr(result));
		return result;
	}

	/* Honor the device-advertised max I/O queue depth. The capability getter returns a zero-based
	 * MQES, so the entry count is mqes + 1. Clamp to the app's compile-time hard ceiling; fall back
	 * to the legacy default if the device cannot report the capability. */
	uint16_t mqes = 0;
	result = doca_devemu_nvme_cap_get_max_io_queue_size(doca_dev_as_devinfo(app->dev), &mqes);
	if (result == DOCA_SUCCESS) {
		uint32_t entries = (uint32_t)mqes + 1;

		if (entries > MEMOS_INITIATOR_REF_APP_MAX_QUEUE_SIZE)
			entries = MEMOS_INITIATOR_REF_APP_MAX_QUEUE_SIZE;
		app->max_io_queue_size = entries;
	} else {
		DOCA_LOG_WARN("Max I/O queue size capability query failed (%s); using default %u entries",
			      doca_error_get_descr(result),
			      MEMOS_INITIATOR_REF_APP_DEFAULT_QUEUE_SIZE);
		app->max_io_queue_size = MEMOS_INITIATOR_REF_APP_DEFAULT_QUEUE_SIZE;
	}
	DOCA_LOG_INFO("Max I/O queue depth: %u entries (CAP.MQES=0x%x)",
		      app->max_io_queue_size,
		      (unsigned)(app->max_io_queue_size - 1));

	result = create_nvme_pci_type(app);
	if (result != DOCA_SUCCESS)
		return result;

	DOCA_LOG_INFO("NVMe pci_type ready");
	return DOCA_SUCCESS;
}

/* Lists every emulated representor managed by the device, regardless of which
 * pci_type / generic_pci_device_type_name it was hotplugged or statically
 * configured under. doca_devinfo_rep_create_list with DOCA_DEVINFO_REP_FILTER_
 * EMULATED bypasses the per-pci_type rep_filter that doca_devemu_pci_type_
 * create_rep_list applies, so this view is name-agnostic. */
doca_error_t memos_initiator_ref_app_list_emulated_functions(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devinfo_rep **rep_list = NULL;
	uint32_t nb_reps = 0;
	uint32_t i;
	doca_error_t result;

	result = open_emulation_manager_dev(app->cfg.pci_address, &app->dev);
	if (result != DOCA_SUCCESS)
		goto out;

	result = doca_devinfo_rep_create_list(app->dev, DOCA_DEVINFO_REP_FILTER_EMULATED, &rep_list, &nb_reps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to enumerate emulated reps for '%s': %s",
			     app->cfg.pci_address,
			     doca_error_get_descr(result));
		goto out;
	}

	DOCA_LOG_INFO("Emulation manager '%s' exposes %u emulated function(s)", app->cfg.pci_address, nb_reps);
	for (i = 0; i < nb_reps; ++i) {
		char rep_vuid[DOCA_DEVINFO_REP_VUID_SIZE] = "<unknown>";
		char rep_pci[DOCA_DEVINFO_REP_PCI_ADDR_SIZE] = "<unknown>";
		enum doca_pci_func_type func_type = DOCA_PCI_FUNC_TYPE_PF;
		uint8_t is_hotplug = 0;
		doca_error_t r;

		r = doca_devinfo_rep_get_vuid(rep_list[i], rep_vuid, sizeof(rep_vuid));
		if (r != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to get VUID of rep %u: %s", i, doca_error_get_descr(r));
		r = doca_devinfo_rep_get_pci_addr_str(rep_list[i], rep_pci);
		if (r != DOCA_SUCCESS)
			DOCA_LOG_WARN("Failed to get PCI address of rep %u: %s", i, doca_error_get_descr(r));
		(void)doca_devinfo_rep_get_pci_func_type(rep_list[i], &func_type);
		(void)doca_devinfo_rep_get_is_hotplug(rep_list[i], &is_hotplug);
		DOCA_LOG_INFO("  [%u] type=%-10s func=%s%s vuid=%-30s pci=%-12s",
			      i,
			      emulation_type_from_vuid(rep_vuid),
			      pci_func_type_label(func_type),
			      is_hotplug ? " hotplug" : "        ",
			      rep_vuid,
			      rep_pci);
	}

out:
	if (rep_list != NULL)
		(void)doca_devinfo_rep_destroy_list(rep_list);
	if (app->dev != NULL) {
		(void)doca_dev_close(app->dev);
		app->dev = NULL;
	}
	app->error = result;
	return result;
}

/* Polls the PE until pci_dev reaches `target` or the wall-clock timeout elapses. The hotplug
 * state-change event is delivered via doca_pe_progress(), so we must keep the PE ticking; the
 * timeout uses CLOCK_MONOTONIC via elapsed_us_since() so unrelated PE traffic cannot starve it. */
static doca_error_t wait_hotplug_state(struct memos_initiator_ref_app_resources *app,
				       struct doca_devemu_pci_dev *pci_dev,
				       enum doca_devemu_pci_hotplug_state target,
				       size_t timeout_us,
				       bool abort_on_force_quit)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_PROGRESS_SLEEP_NS};
	struct timespec start;
	enum doca_devemu_pci_hotplug_state current;
	doca_error_t result;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		result = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &current);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get hotplug state: %s", doca_error_get_descr(result));
			return result;
		}
		if (current == target)
			return DOCA_SUCCESS;
		/* Power-off / unplug are graceful-removal waits triggered *by* force_quit on Ctrl+C, so
		 * they must run to completion (bounded by timeout); only the power-on wait honours the
		 * abort flag so a Ctrl+C during bring-up can break out early. */
		if (abort_on_force_quit && *app->force_quit != 0) {
			DOCA_LOG_ERR("Aborted while waiting for hotplug transition");
			return DOCA_ERROR_OPERATING_SYSTEM;
		}
		if (elapsed_us_since(&start) >= timeout_us) {
			DOCA_LOG_ERR("Timed out waiting for hotplug transition (target=%d, current=%d)",
				     (int)target,
				     (int)current);
			return DOCA_ERROR_TIME_OUT;
		}
		if (memos_initiator_ref_app_progress_all(app) == 0) {
			struct timespec ts = sleep_ts;
			(void)nanosleep(&ts, NULL);
		}
	}
}

/* Make the emulated function visible to the host (idempotent). Called only after the controller is
 * fully set up, so the host never observes a half-initialised device. The underlying pci_dev is
 * owned by app->nvme_dev and is RUNNING by the time we get here. */
static doca_error_t power_on_function(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devemu_pci_dev *pci_dev;
	enum doca_devemu_pci_hotplug_state state;
	doca_error_t result;

	pci_dev = doca_devemu_nvme_dev_as_pci_dev(app->nvme_dev);
	if (pci_dev == NULL) {
		DOCA_LOG_ERR("Failed to get pci_dev from nvme_dev for power-on");
		return DOCA_ERROR_BAD_STATE;
	}

	result = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get hotplug state: %s", doca_error_get_descr(result));
		return result;
	}
	if (state == DOCA_DEVEMU_PCI_HP_STATE_POWER_ON)
		return DOCA_SUCCESS;

	if (state == DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF) {
		DOCA_LOG_INFO("Initiating hotplug power-on (current state=POWER_OFF)");
		result = doca_devemu_pci_dev_hotplug(pci_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initiate hotplug (power-on): %s", doca_error_get_descr(result));
			return result;
		}
	}

	DOCA_LOG_INFO("Waiting for emulated function to reach POWER_ON");
	result = wait_hotplug_state(app,
				    pci_dev,
				    DOCA_DEVEMU_PCI_HP_STATE_POWER_ON,
				    MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US,
				    true);
	if (result == DOCA_SUCCESS)
		DOCA_LOG_INFO("Emulated function is POWER_ON (visible to host)");
	return result;
}

/* Remove the emulated function from the host (idempotent). Used on exit so the host sees an orderly
 * removal; the rep itself is left intact (closed, not destroyed) so a later run can power it on
 * again. A no-op when the controller was never created or is already powered off. */
static doca_error_t power_off_function(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devemu_pci_dev *pci_dev;
	enum doca_devemu_pci_hotplug_state state;
	doca_error_t result;

	if (app->nvme_dev == NULL)
		return DOCA_SUCCESS;

	pci_dev = doca_devemu_nvme_dev_as_pci_dev(app->nvme_dev);
	if (pci_dev == NULL)
		return DOCA_SUCCESS;

	result = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get hotplug state for power-off: %s", doca_error_get_descr(result));
		return result;
	}
	if (state == DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF)
		return DOCA_SUCCESS;

	if (state == DOCA_DEVEMU_PCI_HP_STATE_POWER_ON) {
		result = doca_devemu_pci_dev_hotunplug(pci_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initiate hotunplug (power-off): %s", doca_error_get_descr(result));
			return result;
		}
	}

	result = wait_hotplug_state(app,
				    pci_dev,
				    DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF,
				    MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US,
				    false);
	if (result == DOCA_SUCCESS)
		DOCA_LOG_INFO("Emulated function is POWER_OFF (removed from host)");
	return result;
}

static void hotplug_teardown(struct memos_initiator_ref_app_resources *app,
			     struct doca_devemu_pci_dev **pci_dev,
			     struct doca_dev_rep **rep,
			     bool destroy_rep)
{
	doca_error_t result;

	if (*pci_dev != NULL) {
		struct doca_ctx *ctx = doca_devemu_pci_dev_as_ctx(*pci_dev);

		if (ctx != NULL)
			(void)stop_ctx_and_wait_idle(app, ctx, MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US);
		result = doca_devemu_pci_dev_destroy(*pci_dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy PCI dev: %s", doca_error_get_descr(result));
		*pci_dev = NULL;
	}
	if (*rep != NULL) {
		if (destroy_rep) {
			result = doca_devemu_pci_type_destroy_rep(*rep);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to destroy emulated rep: %s", doca_error_get_descr(result));
		} else {
			result = doca_dev_rep_close(*rep);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR("Failed to close emulated rep: %s", doca_error_get_descr(result));
		}
		*rep = NULL;
	}
	/* The app owns the pci_type; release it before tearing the library down. Destroying the type
	 * software object while a POWER_OFF firmware rep persists is fine - the firmware slot (and its
	 * installed BAR0 defaults) survives for the subsequent --vuid run. */
	destroy_nvme_pci_type(app);
	result = doca_devemu_nvme_teardown();
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to teardown NVMe library: %s", doca_error_get_descr(result));
	if (app->dev != NULL) {
		result = doca_devemu_nvme_rm_dev(app->dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to remove NVMe dev registration: %s", doca_error_get_descr(result));
		result = doca_dev_close(app->dev);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to close DOCA dev: %s", doca_error_get_descr(result));
		app->dev = NULL;
	}
	/* PE must outlive the pci_dev ctx: the IDLE transition above drives events through it. */
	if (app->pe != NULL) {
		result = doca_pe_destroy(app->pe);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy progress engine: %s", doca_error_get_descr(result));
		app->pe = NULL;
	}
}

/* --hotplug: create a new emulated function and leave it POWER_OFF. We install the pci_type BAR0
 * CAP/VS defaults here, while the type is still idle (before create_rep) - the only point the
 * firmware accepts type-level default modification; once a rep exists it returns BAD_RES_STATE. The
 * function then persists in the firmware (discoverable by VUID), and the installed defaults persist
 * with it, so a later regular run (--vuid) powers it on without touching type defaults. We
 * deliberately do NOT power it on here. */
doca_error_t memos_initiator_ref_app_hotplug(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devemu_pci_dev *pci_dev = NULL;
	struct doca_dev_rep *rep = NULL;
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE] = "<unknown>";
	doca_error_t result;

	result = doca_pe_create(&app->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		goto out;
	}

	result = prepare_pci_type_and_dev(app);
	if (result != DOCA_SUCCESS)
		goto out;

	/* Install the BAR0 CAP/VS defaults now, while the type is idle (no rep yet). This is the only
	 * point the firmware accepts type-level default modification; once the rep below exists it
	 * returns BAD_RES_STATE. The defaults persist via the POWER_OFF rep for later --vuid runs.
	 * Non-fatal: the controller is still host-usable through the per-run live CAP/VS seed. */
	result = install_pci_type_bar_defaults(app);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_WARN("BAR0 CAP/VS default install failed (continuing): %s", doca_error_get_descr(result));

	result = doca_devemu_pci_type_create_rep(app->pci_type, &rep);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create rep: %s", doca_error_get_descr(result));
		goto out;
	}

	(void)doca_devinfo_rep_get_vuid(doca_dev_rep_as_devinfo(rep), vuid, sizeof(vuid));
	DOCA_LOG_INFO("Created emulated function VUID=%s (POWER_OFF). Start it with: --pci-addr %s --vuid %s",
		      vuid,
		      app->cfg.pci_address,
		      vuid);

out:
	/* destroy_rep=false: the function must persist in firmware for the subsequent run. */
	hotplug_teardown(app, &pci_dev, &rep, /* destroy_rep */ false);
	app->error = result;
	return result;
}

/* --hotunplug VUID: power the function off if needed and destroy it (remove the firmware slot).
 * Tolerates a function that is already POWER_OFF (e.g. left behind by a regular run that exited). */
doca_error_t memos_initiator_ref_app_hotunplug(struct memos_initiator_ref_app_resources *app)
{
	struct doca_devemu_pci_dev *pci_dev = NULL;
	struct doca_dev_rep *rep = NULL;
	enum doca_devemu_pci_hotplug_state state;
	char vuid[DOCA_DEVINFO_REP_VUID_SIZE] = "<unknown>";
	doca_error_t result;

	if (app->cfg.hotunplug_vuid[0] == '\0') {
		DOCA_LOG_ERR("--hotunplug requires a VUID");
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_pe_create(&app->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		goto out;
	}

	result = prepare_pci_type_and_dev(app);
	if (result != DOCA_SUCCESS)
		goto out;

	/* Name-agnostic lookup so we can unplug reps left behind by an older
	 * library/app version that used a different pci_type name. */
	result = open_emulated_rep_any(app->dev, app->cfg.hotunplug_vuid, &rep);
	if (result != DOCA_SUCCESS)
		goto out;

	(void)doca_devinfo_rep_get_vuid(doca_dev_rep_as_devinfo(rep), vuid, sizeof(vuid));

	result = doca_devemu_pci_rep_get_hotplug_state(rep, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get hotplug state of %s: %s", vuid, doca_error_get_descr(result));
		goto out;
	}

	/* A function that is already powered off is removed by destroying its representor, which needs
	 * no pci_dev. Binding one would require a pci_type matching the function's emulation backing and
	 * name, so this is also the only way to remove a function created under a pci_type this build can
	 * no longer reproduce. */
	if (state == DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF) {
		DOCA_LOG_INFO("VUID=%s is already powered off; destroying its representor", vuid);
		goto destroyed;
	}

	/* Powering a function off means driving it, which needs a pci_type that matches the one it was
	 * created with - and the only type this run has is the one --pci-type asked for. A function
	 * created under a different layout is therefore refused here, with nothing to fall back on, so
	 * say what to do about it instead of reporting the bare error. */
	result = doca_devemu_pci_dev_create(app->pci_type, rep, app->pe, &pci_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pci_dev for %s: %s", vuid, doca_error_get_descr(result));
		DOCA_LOG_ERR(
			"%s is powered on and could not be bound under the requested pci_type; re-run --hotunplug with the --pci-type it was created with",
			vuid);
		goto out;
	}

	result = doca_ctx_start(doca_devemu_pci_dev_as_ctx(pci_dev));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start pci_dev ctx: %s", doca_error_get_descr(result));
		goto out;
	}

	result = doca_devemu_pci_dev_get_hotplug_state(pci_dev, &state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get hotplug state: %s", doca_error_get_descr(result));
		goto out;
	}

	if (state != DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF) {
		DOCA_LOG_INFO("Hotunplug VUID=%s", vuid);
		result = doca_devemu_pci_dev_hotunplug(pci_dev);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initiate hotunplug: %s", doca_error_get_descr(result));
			goto out;
		}
		result = wait_hotplug_state(app,
					    pci_dev,
					    DOCA_DEVEMU_PCI_HP_STATE_POWER_OFF,
					    MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US,
					    false);
		if (result != DOCA_SUCCESS)
			goto out;
	}

destroyed:
	DOCA_LOG_INFO("Hotunplug complete: VUID=%s destroyed", vuid);

out:
	/* destroy_rep only when we reached POWER_OFF cleanly; otherwise just close our handle. */
	hotplug_teardown(app, &pci_dev, &rep, /* destroy_rep */ (result == DOCA_SUCCESS));
	app->error = result;
	return result;
}

/* Start the NVMe device ctx and pump the main PEs until it reaches RUNNING (or force_quit / error).
 * Shared by the initial run path and FLR recovery. */
static doca_error_t start_nvme_ctx_wait_running(struct memos_initiator_ref_app_resources *app, struct doca_ctx *ctx)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_PROGRESS_SLEEP_NS};
	enum doca_ctx_states ctx_state;
	struct timespec start;
	doca_error_t result;

	result = doca_ctx_start(ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to start NVMe ctx: %s", doca_error_get_descr(result));
		return result;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		result = doca_ctx_get_state(ctx, &ctx_state);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query NVMe ctx state: %s", doca_error_get_descr(result));
			return result;
		}
		if (ctx_state == DOCA_CTX_STATE_RUNNING)
			return DOCA_SUCCESS;
		if (ctx_state != DOCA_CTX_STATE_STARTING) {
			DOCA_LOG_ERR("NVMe ctx did not reach RUNNING (state=%d)", (int)ctx_state);
			return DOCA_ERROR_BAD_STATE;
		}
		if (*app->force_quit != 0)
			return DOCA_ERROR_OPERATING_SYSTEM;
		/* Bounded like the other waits here: the context reaching RUNNING depends on firmware
		 * answering, so a lost answer would otherwise spin until the operator interrupts. */
		if (elapsed_us_since(&start) >= MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US) {
			DOCA_LOG_ERR("NVMe ctx did not reach RUNNING within %u s",
				     (unsigned int)(MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US / 1000000));
			return DOCA_ERROR_TIME_OUT;
		}
		if (memos_initiator_ref_app_progress_all(app) == 0) {
			struct timespec ts = sleep_ts;
			(void)nanosleep(&ts, NULL);
		}
	}
}

/* Bring up the per-run datapath on a RUNNING nvme_dev: seed CAP/VS (non-static), cache the host mmap,
 * init the admin DMA engine and start the IO workers. Shared by the initial run path and FLR recovery.
 * The initial-attach "host already active" gate (flag_if_host_active) is NOT part of this: it must run
 * only on first attach, not on FLR recovery (see memos_initiator_ref_app_run / memos_initiator_ref_app_recover_flr). */
static doca_error_t bring_up_datapath(struct memos_initiator_ref_app_resources *app)
{
	doca_error_t result;

	/* Seed CAP/VS into BAR0 (device RUNNING) so the host reads valid capabilities. Skipped for static
	 * functions: firmware configured their read-only BAR0 at boot. */
	if (!app->rep_is_static) {
		result = seed_bar_cap_vs(app);
		if (result != DOCA_SUCCESS)
			return result;
	}

	/* The KV namespace's backing store must exist before any worker can serve a command against it,
	 * and before host_mmap, which registers the initiator's device. It survives controller resets
	 * deliberately: a host that resets its controller has not asked for its data to be discarded. */
	result = memos_initiator_ref_app_kv_init(app);
	if (result != DOCA_SUCCESS)
		return result;

	result = cache_host_mmap(app);
	if (result != DOCA_SUCCESS)
		return result;

	/* The DMA engine resolves admin payload destinations against host_mmap, so bring it up after. */
	result = memos_initiator_ref_app_dma_init(app);
	if (result != DOCA_SUCCESS)
		return result;

	/* Per-core IO workers need a RUNNING nvme_dev; start them before the host can Create-IO-CQ/SQ. */
	return memos_initiator_ref_app_workers_start(app);
}

/* Recover from a host FLR. The firmware tears the emulation object down on FLR and never restores it,
 * so the only path back to an enabled controller is destroy + create. Quiesce the datapath while the
 * (enabled=0) emulation object is still alive - so queue disables and doorbell unmaps target a live
 * object - then stop the device ctx (which destroys the object) and start it again (which creates a
 * fresh enabled=1 object), and bring the datapath back up. The controller comes back disabled (CC=0);
 * the host re-enables it via a normal CC.EN 0->1 write, handled by reg_change_cb as usual. */
static doca_error_t memos_initiator_ref_app_recover_flr(struct memos_initiator_ref_app_resources *app)
{
	struct doca_ctx *ctx = doca_devemu_nvme_dev_as_ctx(app->nvme_dev);
	doca_error_t result;

	app->flr_pending = false;

	DOCA_LOG_WARN("FLR recovery: quiescing datapath and cycling the device context");

	result = quiesce_datapath(app);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("FLR recovery: datapath did not quiesce (%s); not rebuilding it on top of what is left",
			     doca_error_get_descr(result));
		return result;
	}

	result = stop_ctx_and_wait_idle(app, ctx, MEMOS_INITIATOR_REF_APP_HOTPLUG_TIMEOUT_US);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("FLR recovery: failed to stop device ctx: %s", doca_error_get_descr(result));
		return result;
	}

	result = start_nvme_ctx_wait_running(app, ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("FLR recovery: failed to restart device ctx: %s", doca_error_get_descr(result));
		return result;
	}

	result = bring_up_datapath(app);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("FLR recovery: failed to bring up datapath: %s", doca_error_get_descr(result));
		return result;
	}

	/* An FLR is a full controller reset, so no host queue state survives it: drop any
	 * awaiting_reset / CSTS.CFS latched at attach, and reset the registers the previous incarnation
	 * programmed - firmware preserves BAR0 across the emulation-object re-create, and a stale CC
	 * with SHN still set would otherwise be replayed as a fresh shutdown. */
	if (app->awaiting_reset) {
		app->awaiting_reset = false;
		DOCA_LOG_INFO("FLR recovery: cleared awaiting_reset (FLR is a full controller reset)");
	}
	result = reset_host_programmed_regs(app);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("FLR recovery: failed to reset the host-programmed registers: %s",
			     doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("FLR recovery complete; controller re-armed, waiting for host re-enable");
	return DOCA_SUCCESS;
}

doca_error_t memos_initiator_ref_app_run(struct memos_initiator_ref_app_resources *app)
{
	const struct timespec sleep_ts = {.tv_sec = 0, .tv_nsec = MEMOS_INITIATOR_REF_APP_PROGRESS_SLEEP_NS};
	union doca_data user_data;
	doca_error_t result;
	/* Declared here rather than where it is first assigned: the failure paths in between jump to
	 * out, and those jumps would otherwise cross its initialization. */
	struct doca_ctx *ctx;

	user_data.ptr = app;

	result = doca_pe_create(&app->pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create progress engine: %s", doca_error_get_descr(result));
		goto out;
	}

	/* Main-thread admin PE: the admin io ctx (qid 0) and admin DMA ride it; it owns the admin
	 * queues' datapath worker. The IO datapath lives on the per-core worker PEs, created in
	 * memos_initiator_ref_app_workers_start() once nvme_dev is RUNNING. */
	result = doca_pe_create(&app->admin_pe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create admin progress engine: %s", doca_error_get_descr(result));
		goto out;
	}

	result = prepare_pci_type_and_dev(app);
	if (result != DOCA_SUCCESS)
		goto out;

	DOCA_LOG_INFO("Opening emulated representor VUID=%s", app->cfg.vuid);
	result = open_emulated_rep(app->pci_type, app->cfg.vuid, &app->rep);
	if (result != DOCA_SUCCESS)
		goto out;

	{
		uint8_t is_hotplug = 0;

		/* Whether this function is firmware-owned is a property of the function, not a user
		 * choice, so read it from the representor and let it drive power-on and BAR0 seeding. */
		result = doca_devinfo_rep_get_is_hotplug(doca_dev_rep_as_devinfo(app->rep), &is_hotplug);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query whether %s is a hotplug function: %s",
				     app->cfg.vuid,
				     doca_error_get_descr(result));
			goto out;
		}
		app->rep_is_static = (is_hotplug == 0);
		DOCA_LOG_INFO("Function %s is %s",
			      app->cfg.vuid,
			      app->rep_is_static ? "a firmware-owned static function" : "hotplugged");
	}

	DOCA_LOG_INFO("Creating NVMe device");
	result = doca_devemu_nvme_dev_create(app->pci_type, app->rep, app->pe, &app->nvme_dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMe device: %s", doca_error_get_descr(result));
		goto out;
	}

	ctx = doca_devemu_nvme_dev_as_ctx(app->nvme_dev);

	/* The device ctx is pure control plane (BAR / reg-change) and stays on app->pe. The admin
	 * io ctx connects to app->admin_pe in admin_io_bring_up; the per-core IO worker ctxs have
	 * their own PEs created in memos_initiator_ref_app_workers_start(). */
	result = doca_devemu_nvme_dev_event_reg_change_register(app->nvme_dev, reg_change_cb, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register reg-change handler: %s", doca_error_get_descr(result));
		goto out;
	}

	/* FLR is reported by the PCI layer, not as a register change. Registering requires an idle PCI
	 * device, which is why this runs before the context is started; the registration then survives
	 * the stop/start cycle that recovers a reset function. The handler only flags the reset - the
	 * control loop below drives the recovery. */
	result = doca_devemu_pci_dev_event_flr_register(doca_devemu_nvme_dev_as_pci_dev(app->nvme_dev),
							flr_cb,
							user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register FLR handler: %s", doca_error_get_descr(result));
		goto out;
	}

	result = doca_ctx_set_user_data(ctx, user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ctx user data: %s", doca_error_get_descr(result));
		goto out;
	}

	/* Claim the doorbells this run needs: the admin queue pair plus the IO pairs the workers will
	 * serve. The device only accepts this while idle, and a controller that never asks gets none,
	 * so it has to be settled here - before the host driver has anything to talk to. */
	result = memos_initiator_ref_app_plan_io_queues(app);
	if (result != DOCA_SUCCESS)
		goto out;

	result = doca_devemu_nvme_dev_set_num_queue_pairs(app->nvme_dev, (uint16_t)(app->max_io_queues + 1));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set %u queue pair(s) on the NVMe device: %s",
			     app->max_io_queues + 1,
			     doca_error_get_descr(result));
		goto out;
	}

	DOCA_LOG_INFO("Starting NVMe device context (waiting for RUNNING)");
	result = start_nvme_ctx_wait_running(app, ctx);
	if (result != DOCA_SUCCESS)
		goto out;

	DOCA_LOG_INFO("NVMe device context RUNNING");

	/* Seed CAP/VS, cache the host mmap, init the admin DMA and start the IO workers (before power-on,
	 * so the host's Create-IO-CQ/SQ can bind immediately). Shared with FLR recovery. */
	result = bring_up_datapath(app);
	if (result != DOCA_SUCCESS)
		goto out;

	/* Initial attach only, and only for a function a host can already be driving: if a host driver
	 * already enabled the controller (e.g. this app was restarted against a live function), flag
	 * CSTS.CFS and wait for a controller reset instead of adopting stale queues. A function we are
	 * about to power on has no host behind it - whatever BAR0 holds is debris from a previous
	 * incarnation, so reset it and let the host enumerate a clean controller. Both are skipped on
	 * FLR recovery, which resets the registers itself. */
	{
		bool host_visible;

		result = function_is_host_visible(app, &host_visible);
		if (result != DOCA_SUCCESS)
			goto out;

		if (host_visible) {
			result = flag_if_host_active(app);
			if (result != DOCA_SUCCESS)
				goto out;
		} else {
			/* No host is looking yet, but the registers are cleared before one can be: a
			 * function presented with the previous incarnation's BAR0 is what the attach
			 * path above exists to repair. */
			result = reset_host_programmed_regs(app);
			if (result != DOCA_SUCCESS)
				goto out;
		}
	}

	/* Make the function visible to the host. For a hotplugged function we drive the POWER_OFF ->
	 * POWER_ON transition; a static function is already powered on by firmware at boot (and is not
	 * a hotplug device), so we skip the transition and it is host-visible already. CAP/VS were
	 * live-seeded into BAR0 above for hotplug; for static, firmware already configured them. */
	if (app->rep_is_static) {
		DOCA_LOG_INFO("Static function is already powered on by firmware (skipping hotplug power-on)");
	} else {
		DOCA_LOG_INFO("Powering on emulated function (making it visible to host)");
		result = power_on_function(app);
		if (result != DOCA_SUCCESS)
			goto out;
	}

	DOCA_LOG_INFO("MEMOS initiator reference application is up; waiting for host BAR0 writes (Ctrl+C to stop)");

	/* Control-plane steady state on the main thread: pump the device PE (BAR / reg-change: CC.EN,
	 * shutdown, FLR) and the admin PE (admin commands + admin DMA), backing off to a sleep when
	 * both are idle. The IO datapath is driven by the per-core worker threads, not here. */
	while (*app->force_quit == 0) {
		int worked = memos_initiator_ref_app_progress_all(app);

		/* The initiator's keep-alive messages ride its own engine, so it has to be progressed for as long
		 * as the backend is up - not only while a KV command is in flight. Without this the target
		 * drops the connection after ~30 s of host idle and every later command fails EIO. */
		worked += memos_initiator_ref_app_kv_progress(app);

		/* A host FLR is surfaced by flr_cb setting flr_pending. Recover here (not in the
		 * handler): quiesce the datapath and cycle the device ctx so the library re-creates the emulation
		 * object. On recovery failure, abort the run loop and let teardown() clean up. */
		if (app->flr_pending) {
			result = memos_initiator_ref_app_recover_flr(app);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("FLR recovery failed: %s; stopping", doca_error_get_descr(result));
				goto out;
			}
			continue;
		}

		if (worked != 0)
			continue;
		struct timespec ts = sleep_ts;
		(void)nanosleep(&ts, NULL);
	}

out:
	teardown(app);
	app->error = result;
	return result;
}
