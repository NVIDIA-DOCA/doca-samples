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

#ifndef VNET_RECYCLE_H
#define VNET_RECYCLE_H

/*
 * Unified function recycle: a PCIe-level reset invalidates the function's mkey
 * and suspends its MSI-X, so recovery cycles the TLP device and rebuilds the
 * engine. Knows nothing about PFs or VFs; callers describe one kind via ops.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <doca_error.h>

/* The events that require a vNET TLP device to be recycled. One recycle can
 * serve several: a host reboot asserts PERST# while the driver drops Bus
 * Master Enable.
 */
enum vnet_recycle_reason {
	VNET_RECYCLE_FLR = 1u << 0,	       /* Host wrote Initiate FLR in PCIe Device Control */
	VNET_RECYCLE_BME_DISABLE = 1u << 1,    /* Bus Master Enable went 1 -> 0 */
	VNET_RECYCLE_PERST_ASSERTED = 1u << 2, /* PERST# asserted (host reboot / power cycle) */
};

/*
 * FLR and PERST# reset config space architecturally, so the emulated image has
 * to go back to power-on defaults with it; the host re-assigns what it had
 * written. A Bus Master drop resets nothing and is not in this set.
 */
#define VNET_RECYCLE_REASONS_RESET_CFG (VNET_RECYCLE_FLR | VNET_RECYCLE_PERST_ASSERTED)

/* Firmware refuses to restart an endpoint for a short settling window after a
 * host reset. The budget is far larger than that window, so a genuinely broken
 * function is still declared failed rather than retried forever. */
#define VNET_RECYCLE_MAX_ATTEMPTS 500
#define VNET_RECYCLE_RETRY_DELAY_USEC 10000

enum vnet_recycle_phase {
	VNET_RECYCLE_PHASE_IDLE = 0,   /* Nothing in flight */
	VNET_RECYCLE_PHASE_PENDING,    /* Requested; worker has not started the teardown */
	VNET_RECYCLE_PHASE_SUSPENDED,  /* Datapath down and TLP device stopped */
	VNET_RECYCLE_PHASE_PARKED,     /* Suspended, waiting on the host: BME or PERST# */
	VNET_RECYCLE_PHASE_REBUILDING, /* Retrying the TLP device restart and engine rebuild */
	VNET_RECYCLE_PHASE_FAILED,     /* Retry budget spent; host told via NEEDS_RESET */
};

/*
 * Per-function recycle state, embedded in the PF controller and each VF slot.
 * phase is the only latch, and vnet_recycle_step() its only writer once a
 * request is made; the progress-loop sweep can advance any phase.
 */
struct vnet_recycle_state {
	atomic_uint phase;	/* enum vnet_recycle_phase */
	atomic_uint reasons;	/* Bitmask of enum vnet_recycle_reason */
	atomic_uint attempt;	/* Restart attempts spent in the current rebuild */
	atomic_uint generation; /* Bumped per request; stales queued work */
	/* Generation the last completed rebuild served, so an outcome cannot be
	 * honoured once a newer request has taken the function over. */
	atomic_uint completed_generation;
	atomic_bool bme_shadow; /* Last Bus Master Enable value the host wrote */
	/* PERST# asserted. The link stays down for as long as the host is, which
	 * outlasts any retry budget, so a rebuild waits for it to lift. */
	atomic_bool link_down;
	atomic_bool failure_pending;	  /* Worker asks the TLP thread to publish NEEDS_RESET */
	atomic_bool hold_release_pending; /* Worker asks the TLP thread to lift the reset hold */
	/* At most one worker step per function in flight, or the sweep would
	 * queue a fresh one every pass and drain the shared work pool. */
	atomic_bool step_queued;
	atomic_bool tlp_stopped;      /* This sequence has the TLP device stopped */
	_Atomic uint64_t retry_at_ns; /* Monotonic ns to wait until; zero means now */
};

/*
 * One kind of function, from the recycle's point of view. Every callback runs on
 * the worker thread, may block, and must tolerate a function that is already
 * down: a request can arrive mid-flight and the retry path re-enters.
 */
struct vnet_recycle_ops {
	const char *kind; /* Function kind, for logs */
	uint32_t index;	  /* pf_index or VF slot index, for logs */
	void *fn;	  /* Opaque function handle passed back to every callback */
	struct vnet_recycle_state *state;

	/* Tear down the offload engine and everything hanging off it. */
	doca_error_t (*suspend_datapath)(void *fn);
	/* Stop the TLP device; BAD_STATE (already stopped) counts as success. The
	 * object must survive - destroying it leaves config space unanswered. */
	doca_error_t (*stop_tlp)(void *fn);
	/* Restart the TLP device; BAD_STATE (already started) counts as success.
	 * Expected to fail while the firmware settling window is open, so the
	 * step retries rather than giving up. */
	doca_error_t (*start_tlp)(void *fn);
	/* Recreate the offload engine so the next FEATURES_OK builds on a fresh
	 * mkey. Optional - a function with no engine of its own leaves it NULL. */
	doca_error_t (*rebuild_datapath)(void *fn);
	/* Whether the function still exists. Without it a recycle queued against
	 * a function that has since been destroyed would spend its whole budget
	 * failing to restart it. Optional. */
	bool (*is_present)(void *fn);
};

/**
 * @brief Initialise recycle state to idle.
 *
 * @param [in] state: State to initialise.
 */
void vnet_recycle_state_init(struct vnet_recycle_state *state);

/**
 * @brief Latch a recycle request. Runs on the TLP thread.
 *
 * Adds @reason to those already latched. A request arriving mid-recycle folds
 * into it rather than starting a second one, and restarts a rebuild already in
 * progress, since the host has reset the function again.
 *
 * @param [in] ops: Function description.
 * @param [in] reason: Why the function is being recycled.
 * @return true if this started a new recycle, false if an in-flight one
 *         absorbed it. Informational - the sweep queues the work either way.
 */
bool vnet_recycle_request(const struct vnet_recycle_ops *ops, enum vnet_recycle_reason reason);

/**
 * @brief Record a Bus Master Enable write. Runs on the TLP thread.
 *
 * Maintains the BME shadow, and treats a 1->0 transition as a recycle trigger.
 * The 0->1 transition needs nothing done to it here: the sweep polls the shadow,
 * so it picks a parked function back up on its own.
 *
 * @param [in] ops: Function description.
 * @param [in] bme_set: Bus Master Enable as just written by the host.
 * @return true if the drop latched a new recycle request.
 */
bool vnet_recycle_note_bme(const struct vnet_recycle_ops *ops, bool bme_set);

/**
 * @brief Reset the BME shadow to its power-on value. Runs on the TLP thread.
 *
 * A reset clears the Command register architecturally, so the shadow must follow
 * it. A stale shadow makes the driver's re-probe write look like no change, and
 * its transient clear look like a genuine drop.
 *
 * @param [in] state: Function recycle state.
 */
void vnet_recycle_reset_bme_shadow(struct vnet_recycle_state *state);

/**
 * @brief Record whether the PCIe link under this function is down (PERST).
 *        Runs on the TLP thread.
 *
 * Asserting it parks any rebuild; clearing it lets the sweep pick the function
 * back up, so the endpoint is running again before the host finishes enumerating.
 *
 * @param [in] state: Function recycle state.
 * @param [in] down: True on PERST assert, false on deassert.
 */
void vnet_recycle_note_link_down(struct vnet_recycle_state *state, bool down);

/**
 * @brief Advance the recycle by one step. Runs on the worker thread.
 *
 * Holds no lock across a callback, so the caller may drop whatever lock it
 * validated under. Must be paired with vnet_recycle_finish_step(), which releases
 * the step slot and arms the retry deadline.
 *
 * @param [in] ops: Function description.
 * @return DOCA_SUCCESS once rebuilt, or if the function went away;
 *         DOCA_ERROR_IN_PROGRESS to be called again shortly; DOCA_ERROR_AGAIN
 *         while parked; any other error once the retry budget is spent.
 */
doca_error_t vnet_recycle_step(const struct vnet_recycle_ops *ops);

/**
 * @brief Close out a worker step. Runs on the worker thread.
 *
 * Releases the step slot and, when the step asked to be called again, records
 * how long the firmware should be left alone first. The progress-loop sweep
 * takes it from here, so the worker returns to other work immediately.
 *
 * @param [in] state: Function recycle state.
 * @param [in] result: What vnet_recycle_step() returned.
 */
void vnet_recycle_finish_step(struct vnet_recycle_state *state, doca_error_t result);

/**
 * @brief Whether this function needs a worker step queued, and claim the right
 *        to queue it. Runs on the TLP thread from the progress loop.
 *
 * The single re-arm point for the sequence: a dropped work item, an expired retry
 * deadline and a parked function whose bus mastering returned all look the same.
 * On true the caller owns the step slot and must queue or release it.
 *
 * @param [in] ops: Function description.
 * @return true if the caller must queue a worker step.
 */
bool vnet_recycle_needs_step(const struct vnet_recycle_ops *ops);

/**
 * @brief Give back a claimed step slot that could not be queued.
 *
 * @param [in] state: Function recycle state.
 */
void vnet_recycle_release_step(struct vnet_recycle_state *state);

/**
 * @brief Abandon any recycle for a function that is being destroyed.
 *
 * Returns every field to its power-on value, so a slot that is later reused
 * starts clean, and drops the pending host-visible reports, which would
 * otherwise be claimed by a later sweep against a function that no longer
 * exists. Safe to call on an idle state.
 *
 * @param [in] state: Function recycle state.
 */
void vnet_recycle_abandon(struct vnet_recycle_state *state);

/**
 * @brief Claim a pending terminal-failure report. Runs on the TLP thread.
 *
 * @param [in] state: Function recycle state.
 * @return true exactly once per failure, for the caller to publish NEEDS_RESET.
 */
bool vnet_recycle_claim_failure(struct vnet_recycle_state *state);

/**
 * @brief Claim a pending reset-hold release. Runs on the TLP thread.
 *
 * Published from the progress loop rather than on the host's next config access,
 * so a host waiting on the reset still makes progress. Also clears the served
 * reasons; both decisions need the thread that latches requests.
 *
 * @param [in] state: Function recycle state.
 * @return true exactly once per completed recycle, and only while no newer
 *         request has taken the function over.
 */
bool vnet_recycle_claim_hold_release(struct vnet_recycle_state *state);

/**
 * @brief Whether a recycle is in flight for this function.
 *
 * Holds the guest's FEATURES_OK off a function with no engine yet. A
 * failed recycle is deliberately not "in progress": it is waiting to be reset
 * again, and treating it as busy would shut every gate keyed off this for good.
 *
 * @param [in] state: Function recycle state.
 * @return true while a recycle is being worked on.
 */
bool vnet_recycle_in_progress(const struct vnet_recycle_state *state);

/**
 * @brief Whether the progress-loop sweep still has something to do here.
 *
 * Wider than vnet_recycle_in_progress(): also true while a finished recycle has
 * an unclaimed report. Sweep gates must use this, or the gate can close before
 * the report is claimed and strand the reset hold on a recovered function.
 *
 * @param [in] state: Function recycle state.
 * @return true if the sweep must keep looking at this function.
 */
bool vnet_recycle_has_pending_work(const struct vnet_recycle_state *state);

/**
 * @brief Human-readable phase name, for logs.
 *
 * @param [in] phase: enum vnet_recycle_phase value.
 * @return Static string; never NULL.
 */
const char *vnet_recycle_phase_name(unsigned int phase);

#endif /* VNET_RECYCLE_H */
