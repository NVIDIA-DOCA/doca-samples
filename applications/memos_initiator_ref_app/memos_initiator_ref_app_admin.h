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

/*
 * Admin queue lifecycle and the NVMe request dispatcher shared by the admin and IO queues.
 */

#ifndef MEMOS_INITIATOR_REF_APP_ADMIN_H_
#define MEMOS_INITIATOR_REF_APP_ADMIN_H_

#include <doca_error.h>

struct doca_devemu_nvme_io;
struct memos_initiator_ref_app_resources;

/**
 * @brief Bring up the admin queues from the host-programmed BAR0 registers.
 *
 * Called from the CC.EN 0->1 transition (before CSTS.RDY is published). Reads AQA/ACQ/ASQ from
 * BAR0, then creates, configures, starts and enables the admin CQ (qid 0) followed by the admin
 * SQ (qid 0) via the public doca_devemu_nvme_cq / _sq API. Enabling each queue maps its doorbell
 * DUAR, which triggers the firmware's spontaneous first doorbell CQE; the library worker drains
 * both on the next progress tick. Finally it creates the admin io ctx, registers the request
 * handler and binds both admin queues so host admin commands round-trip SQE -> handler -> CQE.
 *
 * @param app  application state with a running nvme_dev.
 * @return DOCA_SUCCESS on success, an error otherwise. Idempotent: no-op if already up.
 */
doca_error_t memos_initiator_ref_app_admin_bring_up(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Register the shared NVMe request dispatcher and app user_data on an io ctx.
 *
 * Used for both the admin io ctx and every per-core worker io ctx so the same dispatcher routes a
 * request to the admin or IO handler by its SQ binding user data. Must be called while the io ctx
 * is idle (before doca_ctx_start).
 *
 * @param app  application state (stored as the io ctx user_data so handlers can recover it).
 * @param io   the io ctx to configure.
 * @return DOCA_SUCCESS on success, an error otherwise.
 */
doca_error_t memos_initiator_ref_app_io_register_dispatch(struct memos_initiator_ref_app_resources *app,
							  struct doca_devemu_nvme_io *io);

/**
 * @brief Complete every held Async-Event-Request with Command Aborted due to SQ Deletion.
 *
 * A held request keeps the admin SQ from draining, so this runs before a device disable is
 * awaited and again from memos_initiator_ref_app_admin_tear_down(). Requires the admin io ctx to
 * still be up. No-op if no request is held.
 *
 * @param app  application state.
 */
void memos_initiator_ref_app_admin_complete_held_aers(struct memos_initiator_ref_app_resources *app);

/**
 * @brief Tear down the admin queues (reverse of memos_initiator_ref_app_admin_bring_up).
 *
 * Unbinds and stops the admin io ctx, then disables, stops and destroys the admin SQ and the
 * admin CQ. Called on CC.EN 1->0 and from teardown() while the nvme_dev is still running. No-op
 * if the admin queues are not up.
 *
 * @param app  application state.
 */
void memos_initiator_ref_app_admin_tear_down(struct memos_initiator_ref_app_resources *app);

#endif /* MEMOS_INITIATOR_REF_APP_ADMIN_H_ */
