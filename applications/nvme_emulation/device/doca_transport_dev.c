/*
 * Copyright (c) 2024-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <doca_dpa_dev_comch_msgq.h>

#include <doca_transport_common.h>

/*
 * RPC for initializing DPA IO thread called before running the thread
 *
 * @db_comp [in]: The DB completion context
 * @cq_db [in]: The CQ DB
 * @consumer [in]: The DPA Comch consumer
 * @return: returns RPC_RETURN_STATUS_SUCCESS on success and RPC_RETURN_STATUS_ERROR otherwise
 */
__dpa_rpc__ uint64_t io_thread_init_rpc(doca_dpa_dev_devemu_pci_db_completion_t db_comp,
					doca_dpa_dev_devemu_pci_db_t cq_db,
					doca_dpa_dev_comch_consumer_t consumer)
{
	int ret = doca_dpa_dev_devemu_pci_db_completion_bind_db(db_comp, cq_db);
	if (ret != 0) {
		DOCA_DPA_DEV_LOG_ERR(
			"Failed to initialize IO DPA thread: Failed to bind CQ to CQ completion context - %d",
			ret);
		return RPC_RETURN_STATUS_ERROR;
	}
	doca_dpa_dev_comch_consumer_ack(consumer, MAX_NUM_COMCH_MSGS);

	return RPC_RETURN_STATUS_SUCCESS;
}

/*
 * Method for handling a message received from DPU
 *
 * @thread_arg [in]: The thread argument
 * @msg [in]: The received message
 */
static void handle_dpu_msg(struct io_thread_arg *thread_arg, const struct comch_msg *msg)
{
	doca_dpa_dev_comch_producer_t producer = thread_arg->dpa_producer;
	doca_dpa_dev_devemu_pci_db_completion_t db_comp = thread_arg->dpa_db_comp;
	struct comch_msg msg_reply;
	doca_dpa_dev_devemu_pci_db_t db;

	switch (msg->type) {
	case COMCH_MSG_TYPE_BIND_SQ_DB:
		db = msg->bind_sq_db_data.db;
		(void)doca_dpa_dev_devemu_pci_db_completion_bind_db(db_comp, db);

		msg_reply = (struct comch_msg){
			.type = COMCH_MSG_TYPE_BIND_SQ_DB_DONE,
			.bind_sq_db_done_data =
				{
					.cookie = msg->bind_sq_db_data.cookie,
				},
		};
		doca_dpa_dev_comch_producer_post_send_imm_only(producer,
							       /*consumer_id=*/1,
							       (const uint8_t *)&msg_reply,
							       sizeof(msg_reply),
							       DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH |
								       DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS);
		break;
	case COMCH_MSG_TYPE_UNBIND_SQ_DB:
		db = msg->unbind_sq_db_data.db;
		(void)doca_dpa_dev_devemu_pci_db_completion_unbind_db(db_comp, db);

		msg_reply = (struct comch_msg){
			.type = COMCH_MSG_TYPE_UNBIND_SQ_DB_DONE,
			.unbind_sq_db_done_data =
				{
					.cookie = msg->unbind_sq_db_data.cookie,
				},
		};
		doca_dpa_dev_comch_producer_post_send_imm_only(producer,
							       /*consumer_id=*/1,
							       (const uint8_t *)&msg_reply,
							       sizeof(msg_reply),
							       DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH |
								       DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS);
		break;
	case COMCH_MSG_TYPE_RAISE_MSIX:
		doca_dpa_dev_devemu_pci_msix_raise(thread_arg->dpa_msix);
		break;
	default:
		break;
	}
}

/*
 * Method for handling all messages received from DPU
 *
 * @thread_arg [in]: The thread argument
 */
static void handle_dpu_msgs(struct io_thread_arg *thread_arg)
{
	doca_dpa_dev_comch_consumer_completion_element_t completion;
	const struct comch_msg *msg;
	uint32_t msg_size;
	doca_dpa_dev_comch_consumer_t consumer = thread_arg->dpa_consumer;
	doca_dpa_dev_comch_consumer_completion_t consumer_comp = thread_arg->dpa_consumer_comp;

	uint32_t num_msgs = 0;
	while (doca_dpa_dev_comch_consumer_get_completion(consumer_comp, &completion) != 0) {
		msg = (const struct comch_msg *)doca_dpa_dev_comch_consumer_get_completion_imm(completion, &msg_size);
		handle_dpu_msg(thread_arg, msg);
		num_msgs++;
	}

	if (num_msgs != 0) {
		doca_dpa_dev_comch_consumer_completion_ack(consumer_comp, num_msgs);
		doca_dpa_dev_comch_consumer_completion_request_notification(consumer_comp);
		doca_dpa_dev_comch_consumer_ack(consumer, num_msgs);
	}
}

#define MAX_DBS 64

struct db_completion_properties {
	doca_dpa_dev_devemu_pci_db_t db;  /**< The DB object */
	doca_dpa_dev_uintptr_t user_data; /**< User data set during initialization of the DB */
};

/*
 * Method for handling a single CQ/SQ DB value received from Host
 *
 * @thread_arg [in]: The thread argument
 * @db [in]: The properties of the received DB
 */
static void handle_db(struct io_thread_arg *thread_arg, struct db_completion_properties *db)
{
	doca_dpa_dev_devemu_pci_db_request_notification(db->db);
	uint32_t db_value = doca_dpa_dev_devemu_pci_db_get_value(db->db);

	struct comch_msg msg = {
		.type = COMCH_MSG_TYPE_HOST_DB,
		.host_db_data =
			{
				.db_user_data = db->user_data,
				.db_value = db_value,
			},
	};
	doca_dpa_dev_comch_producer_post_send_imm_only(thread_arg->dpa_producer,
						       /*consumer_id=*/1,
						       (const uint8_t *)&msg,
						       sizeof(msg),
						       DOCA_DPA_DEV_SUBMIT_FLAG_FLUSH |
							       DOCA_DPA_DEV_SUBMIT_FLAG_OPTIMIZE_REPORTS);
}

/*
 * Method for handling all CQ/SQ DBs received from Host
 *
 * @thread_arg [in]: The thread argument
 */
static void handle_dbs(struct io_thread_arg *thread_arg)
{
	struct db_completion_properties dbs[MAX_DBS];
	uint32_t num_dbs;
	uint32_t db_idx;
	doca_dpa_dev_devemu_pci_db_completion_element_t db_comp;

	num_dbs = 0;
	while (doca_dpa_dev_devemu_pci_get_db_completion(thread_arg->dpa_db_comp, &db_comp) != 0) {
		doca_dpa_dev_devemu_pci_db_completion_element_get_db_properties(thread_arg->dpa_db_comp,
										db_comp,
										&dbs[num_dbs].db,
										&dbs[num_dbs].user_data);
		num_dbs++;
	}

	if (num_dbs != 0) {
		doca_dpa_dev_devemu_pci_db_completion_ack(thread_arg->dpa_db_comp, num_dbs);

		for (db_idx = 0; db_idx < num_dbs; db_idx++) {
			handle_db(thread_arg, &dbs[db_idx]);
		}

		doca_dpa_dev_devemu_pci_db_completion_request_notification(thread_arg->dpa_db_comp);
	}
}

/*
 * The IO thread handler
 *
 * This handler is called whenever one of the following happens:
 * - The consumer has received a message from the DPU
 * - The Host has rung the NVMe CQ doorbell
 * - The Host has rung the NVMe SQ doorbell of SQ
 *
 * @thread_arg_raw [in]: Represents pointer to thread argument initialized on the DPU
 */
__dpa_global__ void io_thread(uint64_t thread_arg_raw)
{
	struct io_thread_arg *thread_arg = (struct io_thread_arg *)thread_arg_raw;

	handle_dpu_msgs(thread_arg);
	handle_dbs(thread_arg);
	doca_dpa_dev_thread_reschedule();
}
