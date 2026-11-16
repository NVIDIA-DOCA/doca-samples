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

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <doca_log.h>

#include "vnet_recycle.h"

DOCA_LOG_REGISTER(VNET_RECYCLE);

#define VNET_RECYCLE_RETRY_DELAY_NS ((uint64_t)VNET_RECYCLE_RETRY_DELAY_USEC * 1000ull)

void vnet_recycle_state_init(struct vnet_recycle_state *state)
{
	if (state == NULL)
		return;

	atomic_init(&state->phase, VNET_RECYCLE_PHASE_IDLE);
	atomic_init(&state->reasons, 0u);
	atomic_init(&state->attempt, 0u);
	atomic_init(&state->generation, 0u);
	atomic_init(&state->completed_generation, 0u);
	/* Bus mastering is off at power-on and stays off until a driver binds. */
	atomic_init(&state->bme_shadow, false);
	atomic_init(&state->link_down, false);
	atomic_init(&state->failure_pending, false);
	atomic_init(&state->hold_release_pending, false);
	atomic_init(&state->step_queued, false);
	/* A function is initialised where its TLP device is created and started:
	 * the PF's at startup, a VF's on VF Enable. */
	atomic_init(&state->tlp_stopped, false);
	atomic_init(&state->retry_at_ns, 0ull);
}

static uint64_t vnet_recycle_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const char *vnet_recycle_phase_name(unsigned int phase)
{
	switch (phase) {
	case VNET_RECYCLE_PHASE_IDLE:
		return "IDLE";
	case VNET_RECYCLE_PHASE_PENDING:
		return "PENDING";
	case VNET_RECYCLE_PHASE_SUSPENDED:
		return "SUSPENDED";
	case VNET_RECYCLE_PHASE_PARKED:
		return "PARKED";
	case VNET_RECYCLE_PHASE_REBUILDING:
		return "REBUILDING";
	case VNET_RECYCLE_PHASE_FAILED:
		return "FAILED";
	default:
		return "UNKNOWN";
	}
}

static const char *vnet_recycle_reasons_str(unsigned int reasons, char *buf, size_t len)
{
	if (buf == NULL || len == 0)
		return "";

	snprintf(buf,
		 len,
		 "0x%x[%s%s%s]",
		 reasons,
		 (reasons & VNET_RECYCLE_FLR) ? "FLR " : "",
		 (reasons & VNET_RECYCLE_BME_DISABLE) ? "BME_DISABLE " : "",
		 (reasons & VNET_RECYCLE_PERST_ASSERTED) ? "PERST#_ASSERTED" : "");
	return buf;
}

bool vnet_recycle_in_progress(const struct vnet_recycle_state *state)
{
	unsigned int phase;

	if (state == NULL)
		return false;

	phase = atomic_load(&state->phase);
	return phase != VNET_RECYCLE_PHASE_IDLE && phase != VNET_RECYCLE_PHASE_FAILED;
}

bool vnet_recycle_has_pending_work(const struct vnet_recycle_state *state)
{
	if (state == NULL)
		return false;

	/* A report the TLP thread has not picked up, or a step that has not run.
	 * step_queued covers the window where the phase already reads idle but
	 * the report it owes is still on its way. */
	if (atomic_load(&state->hold_release_pending) || atomic_load(&state->failure_pending) ||
	    atomic_load(&state->step_queued))
		return true;

	/* A failed recycle deliberately does not count: it has been reported to
	 * the host and needs nothing until the host resets the function, which
	 * arrives as a trigger and re-opens any gate this closed. */
	return vnet_recycle_in_progress(state);
}

static bool vnet_recycle_ops_valid(const struct vnet_recycle_ops *ops)
{
	return ops != NULL && ops->state != NULL && ops->suspend_datapath != NULL && ops->stop_tlp != NULL &&
	       ops->start_tlp != NULL;
}

bool vnet_recycle_request(const struct vnet_recycle_ops *ops, enum vnet_recycle_reason reason)
{
	struct vnet_recycle_state *state;
	unsigned int prev_phase;
	char reasons_buf[48];

	if (!vnet_recycle_ops_valid(ops))
		return false;

	state = ops->state;
	atomic_fetch_or(&state->reasons, (unsigned int)reason);

	/* Bumping the generation is what makes a trigger landing mid-recycle
	 * safe: the running step sees its work is stale, abandons it and starts
	 * over rather than releasing the host into a half-rebuilt function. */
	atomic_fetch_add(&state->generation, 1);

	/* Any phase, including FAILED, restarts from the top. A failed function
	 * that the host resets again is the one chance to recover it. */
	prev_phase = atomic_exchange(&state->phase, VNET_RECYCLE_PHASE_PENDING);
	atomic_store(&state->attempt, 0u);
	/* The firmware settling window restarts with the host's reset, so a
	 * deadline left over from the superseded attempt means nothing. */
	atomic_store(&state->retry_at_ns, 0ull);

	DOCA_LOG_INFO("%s[%u]: recycle requested, reasons=%s (was %s)",
		      ops->kind,
		      ops->index,
		      vnet_recycle_reasons_str(atomic_load(&state->reasons), reasons_buf, sizeof(reasons_buf)),
		      vnet_recycle_phase_name(prev_phase));

	return prev_phase == VNET_RECYCLE_PHASE_IDLE || prev_phase == VNET_RECYCLE_PHASE_FAILED;
}

bool vnet_recycle_note_bme(const struct vnet_recycle_ops *ops, bool bme_set)
{
	struct vnet_recycle_state *state;
	bool was_set;

	if (!vnet_recycle_ops_valid(ops))
		return false;

	state = ops->state;
	was_set = atomic_exchange(&state->bme_shadow, bme_set);
	if (was_set == bme_set)
		return false;

	if (bme_set) {
		/* Updating the shadow is enough: the sweep polls it, so a restore
		 * that lands while the worker is still parking is picked up. */
		DOCA_LOG_DBG("%s[%u]: bus master enabled", ops->kind, ops->index);
		return false;
	}

	DOCA_LOG_INFO("%s[%u]: bus master disabled - recycling function", ops->kind, ops->index);
	vnet_recycle_request(ops, VNET_RECYCLE_BME_DISABLE);
	return true;
}

void vnet_recycle_reset_bme_shadow(struct vnet_recycle_state *state)
{
	if (state == NULL)
		return;

	atomic_store(&state->bme_shadow, false);
}

void vnet_recycle_note_link_down(struct vnet_recycle_state *state, bool down)
{
	if (state == NULL)
		return;

	atomic_store(&state->link_down, down);
}

/*
 * Whether the function may be brought back up now, or must stay parked. Both
 * conditions are re-read on every pass rather than latched, so a Bus Master
 * write or PERST# deassert during the park cannot be missed.
 */
static bool vnet_recycle_rebuild_allowed(const struct vnet_recycle_ops *ops, unsigned int reasons)
{
	if (atomic_load(&ops->state->link_down))
		return false;

	/* A Bus Master drop on its own is the driver detaching, so the rebuild
	 * waits for its re-probe. A drop alongside a reset must not park: the
	 * releasing write cannot arrive until the function is already back. */
	if (reasons == VNET_RECYCLE_BME_DISABLE)
		return atomic_load(&ops->state->bme_shadow);

	return true;
}

bool vnet_recycle_needs_step(const struct vnet_recycle_ops *ops)
{
	struct vnet_recycle_state *state;
	bool unclaimed = false;

	if (!vnet_recycle_ops_valid(ops))
		return false;

	state = ops->state;

	switch (atomic_load(&state->phase)) {
	case VNET_RECYCLE_PHASE_IDLE:
		return false;
	case VNET_RECYCLE_PHASE_FAILED:
		/* Reported to the host and left alone. Only a fresh reset from
		 * the host moves this on, and that arrives as a request. */
		return false;
	case VNET_RECYCLE_PHASE_PARKED:
		if (!vnet_recycle_rebuild_allowed(ops, atomic_load(&state->reasons)))
			return false;
		break;
	default:
		break;
	}

	if (atomic_load(&state->retry_at_ns) > vnet_recycle_now_ns())
		return false;

	return atomic_compare_exchange_strong(&state->step_queued, &unclaimed, true);
}

void vnet_recycle_release_step(struct vnet_recycle_state *state)
{
	if (state == NULL)
		return;

	atomic_store(&state->step_queued, false);
}

void vnet_recycle_finish_step(struct vnet_recycle_state *state, doca_error_t result)
{
	if (state == NULL)
		return;

	if (result == DOCA_ERROR_IN_PROGRESS)
		atomic_store(&state->retry_at_ns, vnet_recycle_now_ns() + VNET_RECYCLE_RETRY_DELAY_NS);
	else
		atomic_store(&state->retry_at_ns, 0ull);

	/* Given back last, so everything this step settled is visible before
	 * the sweep is able to queue another one. */
	atomic_store(&state->step_queued, false);
}

bool vnet_recycle_claim_failure(struct vnet_recycle_state *state)
{
	bool expected = true;

	if (state == NULL)
		return false;

	return atomic_compare_exchange_strong(&state->failure_pending, &expected, false);
}

bool vnet_recycle_claim_hold_release(struct vnet_recycle_state *state)
{
	bool expected = true;

	if (state == NULL)
		return false;

	if (!atomic_compare_exchange_strong(&state->hold_release_pending, &expected, false))
		return false;

	/* Either test failing means the function was reset again since the rebuild
	 * landed, so the recycle now in flight owes the release instead. Neither can
	 * go stale here: this is the thread that latches requests. */
	if (atomic_load(&state->generation) != atomic_load(&state->completed_generation) ||
	    atomic_load(&state->phase) != VNET_RECYCLE_PHASE_IDLE)
		return false;

	/* Every latched reason belongs to the generation just served: a newer
	 * request would have raised the generation with it. */
	atomic_store(&state->reasons, 0u);
	return true;
}

void vnet_recycle_abandon(struct vnet_recycle_state *state)
{
	if (state == NULL)
		return;

	/* step_queued is deliberately left alone: a step may be running now, and
	 * handing the slot back under it would let a second step be queued
	 * against a function that is being freed. */

	/* Bumped so a step already inside a callback treats its work as
	 * superseded and does not publish anything on the way out. */
	atomic_fetch_add(&state->generation, 1);
	atomic_store(&state->reasons, 0u);
	atomic_store(&state->attempt, 0u);
	atomic_store(&state->completed_generation, atomic_load(&state->generation));
	atomic_store(&state->retry_at_ns, 0ull);
	atomic_store(&state->failure_pending, false);
	atomic_store(&state->hold_release_pending, false);
	atomic_store(&state->bme_shadow, false);
	atomic_store(&state->link_down, false);
	atomic_store(&state->tlp_stopped, false);
	/* Stored last, so nothing observes an idle phase alongside state left
	 * over from the recycle being abandoned. */
	atomic_store(&state->phase, VNET_RECYCLE_PHASE_IDLE);
}

/* A newer request has superseded the work this step is doing. */
static bool vnet_recycle_superseded(const struct vnet_recycle_state *state, unsigned int generation)
{
	return atomic_load(&state->generation) != generation;
}

/*
 * Move the phase on, unless a request has overtaken this step. @from is the
 * phase the step did its work in, not a fresh read: a step can come away with a
 * new generation and a stale phase, and only the pair catches both orderings.
 */
static bool vnet_recycle_commit_phase(struct vnet_recycle_state *state,
				      unsigned int generation,
				      unsigned int from,
				      unsigned int to)
{
	unsigned int expected = from;

	if (vnet_recycle_superseded(state, generation))
		return false;

	return atomic_compare_exchange_strong(&state->phase, &expected, to);
}

/*
 * Count one failed attempt and decide whether the sequence continues. Returns
 * DOCA_ERROR_IN_PROGRESS while budget remains, or @result once it is spent,
 * having latched the failure for the TLP thread to report to the host.
 */
static doca_error_t vnet_recycle_retry_or_fail(const struct vnet_recycle_ops *ops,
					       unsigned int generation,
					       unsigned int phase,
					       doca_error_t result,
					       const char *what)
{
	struct vnet_recycle_state *state = ops->state;
	unsigned int attempt = atomic_fetch_add(&state->attempt, 1) + 1;

	if (attempt < VNET_RECYCLE_MAX_ATTEMPTS) {
		/* Expected for the first few attempts while the firmware settling
		 * window is open, so this is not warned about until it is unusual. */
		if (attempt == 1 || (attempt % 50) == 0)
			DOCA_LOG_DBG("%s[%u]: %s not ready (attempt %u/%u): %s",
				     ops->kind,
				     ops->index,
				     what,
				     attempt,
				     VNET_RECYCLE_MAX_ATTEMPTS,
				     doca_error_get_descr(result));
		return DOCA_ERROR_IN_PROGRESS;
	}

	if (!vnet_recycle_commit_phase(state, generation, phase, VNET_RECYCLE_PHASE_FAILED)) {
		/* The host reset the function again, which restarts the budget
		 * and makes this attempt's exhaustion irrelevant. */
		return DOCA_ERROR_IN_PROGRESS;
	}

	DOCA_LOG_ERR("%s[%u]: %s failed after %u attempts (%u ms): %s - reporting NEEDS_RESET to host",
		     ops->kind,
		     ops->index,
		     what,
		     attempt,
		     (attempt * VNET_RECYCLE_RETRY_DELAY_USEC) / 1000,
		     doca_error_get_descr(result));

	atomic_store(&state->failure_pending, true);
	return result;
}

/* Bring the function down: datapath first, then the TLP device. */
static doca_error_t vnet_recycle_suspend(const struct vnet_recycle_ops *ops, unsigned int generation)
{
	const unsigned int phase = VNET_RECYCLE_PHASE_PENDING;
	doca_error_t result;

	result = ops->suspend_datapath(ops->fn);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
		return vnet_recycle_retry_or_fail(ops, generation, phase, result, "datapath suspend");

	/* Asking the SDK to stop what this sequence stopped earlier only earns
	 * an error line for a state it is already in; see tlp_stopped. */
	if (atomic_load(&ops->state->tlp_stopped))
		return DOCA_SUCCESS;

	result = ops->stop_tlp(ops->fn);
	/* BAD_STATE is the SDK saying it is already stopped, which is exactly
	 * where this step is trying to get to. */
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
		return vnet_recycle_retry_or_fail(ops, generation, phase, result, "TLP device stop");

	atomic_store(&ops->state->tlp_stopped, true);
	return DOCA_SUCCESS;
}

/* Bring the function back up: restart the TLP device, then rebuild the engine
 * on the other side of it so the next FEATURES_OK gets a live mkey. */
static doca_error_t vnet_recycle_rebuild(const struct vnet_recycle_ops *ops, unsigned int generation)
{
	const unsigned int phase = VNET_RECYCLE_PHASE_REBUILDING;
	doca_error_t result;

	result = ops->start_tlp(ops->fn);
	/* BAD_STATE is the SDK saying it is already started, which is where this
	 * step is trying to get to - the mirror of the stop above. */
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
		return vnet_recycle_retry_or_fail(ops, generation, phase, result, "TLP device start");

	/* Cleared only on success, so a rebuild that lost the firmware race
	 * retries the start rather than trying to stop a stopped device first. */
	atomic_store(&ops->state->tlp_stopped, false);

	if (ops->rebuild_datapath != NULL) {
		result = ops->rebuild_datapath(ops->fn);
		if (result != DOCA_SUCCESS)
			return vnet_recycle_retry_or_fail(ops, generation, phase, result, "datapath rebuild");
	}

	return DOCA_SUCCESS;
}

/*
 * A phase commit fails only when a concurrent request moved the state under this
 * step, so the work it did is discarded and the step runs again. Announced
 * because its side effects otherwise read like a double teardown.
 */
static doca_error_t vnet_recycle_note_supersede(const struct vnet_recycle_ops *ops, unsigned int from, unsigned int to)
{
	DOCA_LOG_INFO("%s[%u]: step superseded committing %s -> %s, restarting against the newer request",
		      ops->kind,
		      ops->index,
		      vnet_recycle_phase_name(from),
		      vnet_recycle_phase_name(to));
	return DOCA_ERROR_IN_PROGRESS;
}

doca_error_t vnet_recycle_step(const struct vnet_recycle_ops *ops)
{
	struct vnet_recycle_state *state;
	unsigned int generation;
	unsigned int reasons;
	unsigned int phase;
	doca_error_t result;

	if (!vnet_recycle_ops_valid(ops))
		return DOCA_ERROR_INVALID_VALUE;

	state = ops->state;
	/* This snapshot can tear against a concurrent request, so nothing below
	 * assumes the three agree: every transition is committed against the
	 * phase this step believed it was in, and a torn read just retries. */
	phase = atomic_load(&state->phase);
	reasons = atomic_load(&state->reasons);
	generation = atomic_load(&state->generation);

	if (phase == VNET_RECYCLE_PHASE_IDLE || phase == VNET_RECYCLE_PHASE_FAILED)
		return DOCA_SUCCESS;

	if (ops->is_present != NULL && !ops->is_present(ops->fn)) {
		DOCA_LOG_INFO("%s[%u]: recycle abandoned in %s - the function no longer exists",
			      ops->kind,
			      ops->index,
			      vnet_recycle_phase_name(phase));
		vnet_recycle_abandon(state);
		return DOCA_SUCCESS;
	}

	if (phase == VNET_RECYCLE_PHASE_PENDING) {
		result = vnet_recycle_suspend(ops, generation);
		if (result != DOCA_SUCCESS)
			return result;

		if (!vnet_recycle_commit_phase(state, generation, phase, VNET_RECYCLE_PHASE_SUSPENDED))
			return vnet_recycle_note_supersede(ops, phase, VNET_RECYCLE_PHASE_SUSPENDED);

		atomic_store(&state->attempt, 0u);
		phase = VNET_RECYCLE_PHASE_SUSPENDED;
		DOCA_LOG_INFO("%s[%u]: suspended - datapath torn down, TLP device stopped", ops->kind, ops->index);
	}

	if (phase == VNET_RECYCLE_PHASE_SUSPENDED || phase == VNET_RECYCLE_PHASE_PARKED) {
		if (!vnet_recycle_rebuild_allowed(ops, reasons)) {
			if (phase != VNET_RECYCLE_PHASE_PARKED &&
			    vnet_recycle_commit_phase(state, generation, phase, VNET_RECYCLE_PHASE_PARKED))
				DOCA_LOG_INFO("%s[%u]: parked - waiting for %s",
					      ops->kind,
					      ops->index,
					      atomic_load(&state->link_down) ? "PERST to deassert" :
									       "the host to restore bus mastering");
			return DOCA_ERROR_AGAIN;
		}

		if (!vnet_recycle_commit_phase(state, generation, phase, VNET_RECYCLE_PHASE_REBUILDING))
			return vnet_recycle_note_supersede(ops, phase, VNET_RECYCLE_PHASE_REBUILDING);

		atomic_store(&state->attempt, 0u);
	}

	result = vnet_recycle_rebuild(ops, generation);
	if (result != DOCA_SUCCESS)
		return result;

	if (!vnet_recycle_commit_phase(state, generation, VNET_RECYCLE_PHASE_REBUILDING, VNET_RECYCLE_PHASE_IDLE))
		return vnet_recycle_note_supersede(ops, VNET_RECYCLE_PHASE_REBUILDING, VNET_RECYCLE_PHASE_IDLE);

	atomic_store(&state->attempt, 0u);

	/* Record what this run served and ask for the release; the sweep decides
	 * whether to honour it. Set before the flag, so nothing sees the function
	 * finished while what it owes the host is not yet on the books. */
	atomic_store(&state->completed_generation, generation);
	atomic_store(&state->hold_release_pending, true);

	DOCA_LOG_INFO("%s[%u]: recycle complete - TLP device restarted and datapath rebuilt", ops->kind, ops->index);
	return DOCA_SUCCESS;
}
