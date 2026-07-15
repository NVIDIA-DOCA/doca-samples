/*
 * Copyright (c) 2022 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_dpa_dev.h>
#include <doca_dpa_dev_devemu_pci.h>

struct {
	doca_dpa_dev_devemu_pci_db_completion_t db_comp; /**< The DB completion context */
} app_ctx;

/*
 * RPC function for initializing the DPA app context
 *
 * @db_comp [in]: The Doorbell completion context
 * @db [in]: The Doorbell object
 * @return: 0 in case of success, and 1 otherwise
 */
__dpa_rpc__ uint64_t init_app_ctx_rpc(doca_dpa_dev_devemu_pci_db_completion_t db_comp, doca_dpa_dev_devemu_pci_db_t db)
{
	app_ctx.db_comp = db_comp;

	if (doca_dpa_dev_devemu_pci_db_completion_bind_db(app_ctx.db_comp, db) < 0) {
		DOCA_DPA_DEV_LOG_INFO("Failed to bind DB to completion \n");
		return 1;
	}

	return 0;
}

/*
 * RPC function for uninitializing the DPA app context
 *
 * @db_comp [in]: The Doorbell completion context
 * @db [in]: The Doorbell object
 * @return: 0 in case of success, and 1 otherwise
 */
__dpa_rpc__ uint64_t uninit_app_ctx_rpc(doca_dpa_dev_devemu_pci_db_completion_t db_comp,
					doca_dpa_dev_devemu_pci_db_t db)
{
	if (doca_dpa_dev_devemu_pci_db_completion_unbind_db(db_comp, db) < 0) {
		DOCA_DPA_DEV_LOG_INFO("Failed to unbind DB from completion \n");
		return 1;
	}

	app_ctx.db_comp = 0;

	return 0;
}

/*
 * Doorbell handler wakes up everytime a DB is rang from Host
 *
 * @thread_arg [in]: Value provided on setup of DPA thread
 */
__dpa_global__ void db_handler(uint64_t __attribute__((__unused__)) thread_arg)
{
	doca_dpa_dev_devemu_pci_db_completion_t db_comp = app_ctx.db_comp;

	doca_dpa_dev_devemu_pci_db_completion_element_t comp_element;
	if (doca_dpa_dev_devemu_pci_get_db_completion(db_comp, &comp_element) != 1) {
		DOCA_DPA_DEV_LOG_INFO("No completion found\n");
		doca_dpa_dev_thread_reschedule();
	}

	uint32_t dbr_value;
	doca_dpa_dev_devemu_pci_db_t db;
	doca_dpa_dev_uintptr_t user_data;
	doca_dpa_dev_devemu_pci_db_completion_element_get_db_properties(db_comp, comp_element, &db, &user_data);

	doca_dpa_dev_devemu_pci_db_completion_ack(db_comp, /*num_comp=*/1);
	doca_dpa_dev_devemu_pci_db_completion_request_notification(db_comp);

	doca_dpa_dev_devemu_pci_db_request_notification(db);

	dbr_value = doca_dpa_dev_devemu_pci_db_get_value(db);
	DOCA_DPA_DEV_LOG_INFO("Received Doorbell value is %u\n", dbr_value);

	doca_dpa_dev_thread_reschedule();
}
