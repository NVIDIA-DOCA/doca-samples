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

#include "nvmf_doca_io.h"
#include "nvme_pci_type_config.h"
#include <doca_transport_common.h>

#include <spdk/util.h>

#include <doca_log.h>

DOCA_LOG_REGISTER(NVME_PCI_COMMON);

extern doca_dpa_func_t io_thread;
extern doca_dpa_func_t io_thread_init_rpc;

#define CACHELINE_SIZE_BYTES 64

/*
 * Method invoked once bind DB done message is received from DPA
 *
 * @msg [in]: The bind done message that was received from DPA
 */
static void nvmf_doca_io_handle_bind_sq_db_done_msg(const struct comch_msg *msg);

/*
 * Method invoked once unbind DB done message is received from DPA
 *
 * @msg [in]: The unbind done message that was received from DPA
 */
static void nvmf_doca_io_handle_unbind_sq_db_done_msg(const struct comch_msg *msg);

/*
 * Method invoked once Host DB message is received from DPA
 *
 * @io [in]: The IO used to receive the message
 * @msg [in]: The Host DB message that was received from DPA
 */
static void nvmf_doca_io_handle_host_db_msg(struct nvmf_doca_io *io, const struct comch_msg *msg);

/*
 * Continue the async flow of stopping the communication channel
 *
 * @comch [in]: The NVMf DOCA DPA communication channel to stop
 */
static void nvmf_doca_dpa_comch_stop_continue(struct nvmf_doca_dpa_comch *comch);

/*
 * Continue the async flow of stopping the IO
 *
 * @io [in]: The NVMf DOCA IO to stop
 */
static void nvmf_doca_io_stop_continue(struct nvmf_doca_io *io);

/*
 * Continue the async flow of stopping the CQ
 *
 * @cq [in]: The NVMf DOCA CQ to stop
 */
static void nvmf_doca_cq_stop_continue(struct nvmf_doca_cq *cq);

/*
 * Continue the async flow of stopping the SQ
 *
 * @sq [in]: The NVMf DOCA SQ to stop
 */
static void nvmf_doca_sq_stop_continue(struct nvmf_doca_sq *sq);

/*
 * Method invoked once a message is received from DPA
 *
 * @io [in]: The IO used to receive the message
 * @msg [in]: The message that was received from DPA
 */
static void nvmf_doca_io_handle_dpa_msg(struct nvmf_doca_io *io, const struct comch_msg *msg)
{
	switch (msg->type) {
	case COMCH_MSG_TYPE_BIND_SQ_DB_DONE:
		nvmf_doca_io_handle_bind_sq_db_done_msg(msg);
		break;
	case COMCH_MSG_TYPE_UNBIND_SQ_DB_DONE:
		nvmf_doca_io_handle_unbind_sq_db_done_msg(msg);
		break;
	case COMCH_MSG_TYPE_HOST_DB:
		nvmf_doca_io_handle_host_db_msg(io, msg);
		break;
	default:
		DOCA_LOG_ERR("Failed to handle DPA message: Received unknown message type %d", msg->type);
		break;
	}
}

/*
 * Callback invoked once a message is received from DPA successfully
 *
 * @recv_task [in]: The receive task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the consumer context
 */
static void nvmf_doca_dpa_msgq_recv_cb(struct doca_comch_consumer_task_post_recv *recv_task,
				       union doca_data task_user_data,
				       union doca_data ctx_user_data)
{
	(void)task_user_data;

	doca_error_t result;

	struct nvmf_doca_io *io = ctx_user_data.ptr;
	struct comch_msg msg = *(struct comch_msg *)doca_comch_consumer_task_post_recv_get_imm_data(recv_task);
	struct doca_task *task = doca_comch_consumer_task_post_recv_as_task(recv_task);

	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("DPA MsgQ receive callback failed: Failed to resubmit receive task - %s",
			     doca_error_get_name(result));
		doca_task_free(task);
	}

	nvmf_doca_io_handle_dpa_msg(io, &msg);
}

/*
 * Callback invoked once consumer encounters a receive error
 *
 * @recv_task [in]: The receive task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the consumer context
 */
static void nvmf_doca_dpa_msgq_recv_error_cb(struct doca_comch_consumer_task_post_recv *recv_task,
					     union doca_data task_user_data,
					     union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	struct doca_task *task = doca_comch_consumer_task_post_recv_as_task(recv_task);

	doca_task_free(task);
}

/*
 * Callback invoked once a message is sent to DPA successfully
 *
 * @send_task [in]: The send task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the producer context
 */
static void nvmf_doca_dpa_msgq_send_cb(struct doca_comch_producer_task_send *send_task,
				       union doca_data task_user_data,
				       union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	struct doca_task *task = doca_comch_producer_task_send_as_task(send_task);

	doca_task_free(task);
}

/*
 * Callback invoked once producer encounters a send error
 *
 * @send_task [in]: The send task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the producer context
 */
static void nvmf_doca_dpa_msgq_send_error_cb(struct doca_comch_producer_task_send *send_task,
					     union doca_data task_user_data,
					     union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	struct doca_task *task = doca_comch_producer_task_send_as_task(send_task);

	doca_task_free(task);
}

/*
 * Callback invoked once consumer/producer state changes
 *
 * @user_data [in]: The user data associated with the context
 * @ctx [in]: The consumer/producer context
 * @prev_state [in]: The previous state
 * @next_state [in]: The new state
 */
static void nvmf_doca_dpa_comch_msgq_ctx_state_changed_cb(const union doca_data user_data,
							  struct doca_ctx *ctx,
							  enum doca_ctx_states prev_state,
							  enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct nvmf_doca_io *io = user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		nvmf_doca_dpa_comch_stop_continue(&io->comch);
		break;
	case DOCA_CTX_STATE_STARTING:
	case DOCA_CTX_STATE_RUNNING:
	case DOCA_CTX_STATE_STOPPING:
	default:
		break;
	}
}

struct nvmf_doca_dpa_msgq_create_attr {
	struct doca_dev *dev; /**< A doca device representing the emulation manager */
	struct doca_dpa *dpa; /**< DOCA DPA for accessing DPA resources */
	bool is_send;	      /**< If MsgQ is used to send to DPA or receive from DPA */
	uint32_t max_num_msg; /**< The maximal number of messages that can be sent/received */
	struct doca_comch_consumer_completion *consumer_comp; /**< Consumer completion context used by DPA to poll
								 arrival of messages */
	struct doca_dpa_completion *producer_comp; /**< Producer completion context used by DPA to poll completion of
						      send message */
	struct doca_pe *pe;			   /**< Progress engine to be used by DPU consumer and producer */
	doca_ctx_state_changed_callback_t ctx_state_changed_cb; /**< Callback invoked once consumer/producer state
								   changes */
	void *ctx_user_data; /**< The user data to associate with the producer/consumer */
};

/*
 * Create NVMf DOCA DPA MsgQ for single direction communication with DPA
 *
 * @attr [in]: The MsgQ attributes
 * @msgq [out]: The newly created MsgQ
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_dpa_msgq_create(const struct nvmf_doca_dpa_msgq_create_attr *attr,
					      struct nvmf_doca_dpa_msgq *msgq)
{
	struct doca_ctx *consumer_ctx;
	struct doca_ctx *producer_ctx;
	doca_error_t result;

	memset(msgq, 0, sizeof(*msgq));

	msgq->is_send = attr->is_send;

	result = doca_comch_msgq_create(attr->dev, &msgq->msgq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to create MsgQ - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_comch_msgq_set_max_num_consumers(msgq->msgq, /*max_num_consumers=*/1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to set max number of consumers - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_comch_msgq_set_max_num_producers(msgq->msgq, /*max_num_producers=*/1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to set max number of producers - %s",
			     doca_error_get_name(result));
		return result;
	}
	if (attr->is_send) {
		result = doca_comch_msgq_set_dpa_consumer(msgq->msgq, attr->dpa);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to failed to set consumer to DPA - %s",
				doca_error_get_name(result));
			return result;
		}
	} else {
		result = doca_comch_msgq_set_dpa_producer(msgq->msgq, attr->dpa);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to failed to set producer to DPA - %s",
				doca_error_get_name(result));
			return result;
		}
	}
	result = doca_comch_msgq_start(msgq->msgq);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to start MsgQ - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_comch_msgq_consumer_create(msgq->msgq, &msgq->consumer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to create consumer - %s",
			     doca_error_get_name(result));
		return result;
	}
	consumer_ctx = doca_comch_consumer_as_ctx(msgq->consumer);
	result = doca_comch_consumer_set_imm_data_len(msgq->consumer, sizeof(struct comch_msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to set consumer immediate data length - %s",
			     doca_error_get_name(result));
		return result;
	}
	if (attr->is_send) {
		/* Consumer on DPA */
		result = doca_ctx_set_datapath_on_dpa(consumer_ctx, attr->dpa);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set consumer data path to DPA - %s",
				doca_error_get_name(result));
			return result;
		}
		result = doca_comch_consumer_set_completion(msgq->consumer, attr->consumer_comp, /*user_data=*/0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to set consumer completion - %s",
				     doca_error_get_name(result));
			return result;
		}
		result = doca_comch_consumer_set_dev_max_num_recv(msgq->consumer, attr->max_num_msg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set consumer max number of receive messages - %s",
				doca_error_get_name(result));
			return result;
		}
	} else {
		/* Consumer on DPU */
		union doca_data ctx_user_data;
		ctx_user_data.ptr = attr->ctx_user_data;
		result = doca_ctx_set_user_data(consumer_ctx, ctx_user_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to set consumer user data - %s",
				     doca_error_get_name(result));
			return result;
		}
		result = doca_ctx_set_state_changed_cb(consumer_ctx, attr->ctx_state_changed_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set consumer context state changed callback - %s",
				doca_error_get_name(result));
			return result;
		}
		result = doca_pe_connect_ctx(attr->pe, consumer_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to connect consumer to progress engine - %s",
				doca_error_get_name(result));
			return result;
		}
		result = doca_comch_consumer_task_post_recv_set_conf(msgq->consumer,
								     nvmf_doca_dpa_msgq_recv_cb,
								     nvmf_doca_dpa_msgq_recv_error_cb,
								     attr->max_num_msg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set consumer task configurations - %s",
				doca_error_get_name(result));
			return result;
		}
	}
	result = doca_ctx_start(consumer_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to start consumer - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_comch_msgq_producer_create(msgq->msgq, &msgq->producer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to create producer - %s",
			     doca_error_get_name(result));
		return result;
	}
	producer_ctx = doca_comch_producer_as_ctx(msgq->producer);
	if (attr->is_send) {
		/* Producer on DPU */
		union doca_data ctx_user_data;
		ctx_user_data.ptr = attr->ctx_user_data;
		result = doca_ctx_set_user_data(producer_ctx, ctx_user_data);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to set producer user data - %s",
				     doca_error_get_name(result));
			return result;
		}
		result = doca_ctx_set_state_changed_cb(producer_ctx, attr->ctx_state_changed_cb);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set producer context state changed callback - %s",
				doca_error_get_name(result));
			return result;
		}
		result = doca_pe_connect_ctx(attr->pe, producer_ctx);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to connect producer to progress engine - %s",
				doca_error_get_name(result));
			return result;
		}
		result = doca_comch_producer_task_send_set_conf(msgq->producer,
								nvmf_doca_dpa_msgq_send_cb,
								nvmf_doca_dpa_msgq_send_error_cb,
								attr->max_num_msg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set producer task configurations - %s",
				doca_error_get_name(result));
			return result;
		}
	} else {
		/* Producer on DPA */
		result = doca_ctx_set_datapath_on_dpa(producer_ctx, attr->dpa);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set producer data path to DPA - %s",
				doca_error_get_name(result));
			return result;
		}
		result = doca_comch_producer_set_dev_max_num_send(msgq->producer, attr->max_num_msg);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA MsgQ: Failed to set producer max number of send messages - %s",
				doca_error_get_name(result));
			return result;
		}
		result = doca_comch_producer_dpa_completion_attach(msgq->producer, attr->producer_comp);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to set producer completion - %s",
				     doca_error_get_name(result));
			return result;
		}
	}
	result = doca_ctx_start(producer_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA MsgQ: Failed to start producer - %s",
			     doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Stop the NVMf DOCA DPA MsgQ
 *
 * This operation is async, once operation completes then nvmf_doca_dpa_comch_msgq_ctx_state_changed_cb() will be
 * invoked
 *
 * @msgq [in]: The NVMf DOCA DPA MsgQ to stop
 */
static void nvmf_doca_dpa_msgq_stop(struct nvmf_doca_dpa_msgq *msgq)
{
	struct doca_ctx *ctx;
	doca_error_t result;

	if (msgq->is_send) {
		ctx = doca_comch_producer_as_ctx(msgq->producer);
	} else {
		ctx = doca_comch_consumer_as_ctx(msgq->consumer);
	}

	result = doca_ctx_stop(ctx);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA MsgQ: Failed to stop context - %s", doca_error_get_name(result));
	}
}

/*
 * Destroy NVMf DOCA DPA MsgQ
 *
 * @msgq [in]: The MsgQ to destroy
 */
static void nvmf_doca_dpa_msgq_destroy(struct nvmf_doca_dpa_msgq *msgq)
{
	doca_error_t result;

	if (msgq->producer != NULL) {
		result = doca_ctx_stop(doca_comch_producer_as_ctx(msgq->producer));
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA MsgQ: Failed to stop producer - %s",
				     doca_error_get_name(result));
		}
		result = doca_comch_producer_destroy(msgq->producer);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA MsgQ: Failed to destroy producer - %s",
				     doca_error_get_name(result));
		}
		msgq->producer = NULL;
	}

	if (msgq->consumer != NULL) {
		result = doca_ctx_stop(doca_comch_consumer_as_ctx(msgq->consumer));
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA MsgQ: Failed to stop consumer - %s",
				     doca_error_get_name(result));
		}
		result = doca_comch_consumer_destroy(msgq->consumer);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA MsgQ: Failed to destroy consumer - %s",
				     doca_error_get_name(result));
		}
		msgq->consumer = NULL;
	}

	if (msgq->msgq != NULL) {
		result = doca_comch_msgq_stop(msgq->msgq);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA MsgQ: Failed to stop MsgQ - %s",
				     doca_error_get_name(result));
		}
		result = doca_comch_msgq_destroy(msgq->msgq);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA MsgQ: Failed to destroy MsgQ - %s",
				     doca_error_get_name(result));
		}
		msgq->msgq = NULL;
	}
}

/*
 * Send message to DPA using NVMf DOCA DPA MsgQ
 *
 * @msgq [in]: The MsgQ to be used for the send operation
 * @msg [in]: The message to send
 * @msg_size [in]: The message size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_dpa_msgq_send(struct nvmf_doca_dpa_msgq *msgq, void *msg, uint32_t msg_size)
{
	doca_error_t result;

	struct doca_comch_producer_task_send *send_task;
	result = doca_comch_producer_task_send_alloc_init(msgq->producer,
							  NULL,
							  msg,
							  msg_size,
							  /*consumer_id=*/1,
							  &send_task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send msg using NVMf DOCA DPA MsgQ: Failed to allocate send task - %s",
			     doca_error_get_name(result));
		return result;
	}

	struct doca_task *task = doca_comch_producer_task_send_as_task(send_task);
	result = doca_task_submit(task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to send msg using NVMf DOCA DPA MsgQ: Failed to submit send task - %s",
			     doca_error_get_name(result));
		doca_task_free(task);
		return result;
	}

	return DOCA_SUCCESS;
}

struct nvmf_doca_dpa_comch_create_attr {
	struct doca_dev *dev;		    /**< A doca device representing the emulation manager */
	struct doca_dpa *dpa;		    /**< DOCA DPA for accessing DPA resources */
	struct doca_dpa_thread *dpa_thread; /**< the DOCA DPA thread to communicate with */
	uint32_t max_num_msg;		    /**< The maximal number of messages that can be sent/received */
	struct doca_pe *pe;		    /**< Progress engine to be used by DPU consumer and producer */
	void *ctx_user_data;		    /**< The user data to associate with the producer/consumer */
};

/*
 * Create NVMf DOCA DPA communication channel for full-duplex communication with DPA
 *
 * @attr [in]: The communication channel attributes
 * @comch [out]: The newly created communication channel
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_dpa_comch_create(const struct nvmf_doca_dpa_comch_create_attr *attr,
					       struct nvmf_doca_dpa_comch *comch)
{
	doca_error_t result;

	memset(comch, 0, sizeof(*comch));

	result = doca_comch_consumer_completion_create(&comch->consumer_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA comch: Failed to create consumer completion - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_comch_consumer_completion_set_max_num_recv(comch->consumer_comp, attr->max_num_msg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create NVMf DOCA DPA comch: Failed to set consumer completion max number of receive messages - %s",
			doca_error_get_name(result));
		return result;
	}
	result = doca_comch_consumer_completion_set_imm_data_len(comch->consumer_comp, sizeof(struct comch_msg));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create NVMf DOCA DPA comch: Failed to set consumer completion immediate data length - %s",
			doca_error_get_name(result));
		return result;
	}
	result = doca_comch_consumer_completion_set_dpa_thread(comch->consumer_comp, attr->dpa_thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA comch: Failed to set consumer completion DPA thread - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_comch_consumer_completion_start(comch->consumer_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA comch: Failed to start consumer completion - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_dpa_completion_create(attr->dpa, attr->max_num_msg, &comch->producer_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA comch: Failed to create producer completion - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_dpa_completion_set_thread(comch->producer_comp, attr->dpa_thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA comch: Failed to set producer completion DPA thread - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_dpa_completion_start(comch->producer_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA comch: Failed to start producer completion - %s",
			     doca_error_get_name(result));
		return result;
	}

	struct nvmf_doca_dpa_msgq_create_attr msgq_attr = {
		.dev = attr->dev,
		.dpa = attr->dpa,
		.max_num_msg = attr->max_num_msg,
		.consumer_comp = comch->consumer_comp,
		.producer_comp = comch->producer_comp,
		.pe = attr->pe,
		.ctx_state_changed_cb = nvmf_doca_dpa_comch_msgq_ctx_state_changed_cb,
		.ctx_user_data = attr->ctx_user_data,
	};
	msgq_attr.is_send = true;
	result = nvmf_doca_dpa_msgq_create(&msgq_attr, &comch->send);
	if (result != DOCA_SUCCESS)
		return result;

	msgq_attr.is_send = false;
	result = nvmf_doca_dpa_msgq_create(&msgq_attr, &comch->recv);
	if (result != DOCA_SUCCESS)
		return result;

	for (uint32_t idx = 0; idx < attr->max_num_msg; idx++) {
		struct doca_comch_consumer_task_post_recv *recv_task;
		result = doca_comch_consumer_task_post_recv_alloc_init(comch->recv.consumer, NULL, &recv_task);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA comch: Failed to allocate receive message at idx %u - %s",
				idx,
				doca_error_get_name(result));
			return result;
		}
		struct doca_task *task = doca_comch_consumer_task_post_recv_as_task(recv_task);
		result = doca_task_submit(task);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA DPA comch: Failed to submit receive message at idx %u - %s",
				idx,
				doca_error_get_name(result));
			doca_task_free(task);
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Check if a DOCA context state is idle
 *
 * @ctx [in]: The DOCA context to check
 * @return: true in case context is idle and false otherwise
 */
static bool is_ctx_idle(struct doca_ctx *ctx)
{
	enum doca_ctx_states ctx_state;
	doca_error_t result;

	result = doca_ctx_get_state(ctx, &ctx_state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to check if context is idle: Failed to get context state - %s",
			     doca_error_get_name(result));
		return false;
	}

	return ctx_state == DOCA_CTX_STATE_IDLE;
}

/*
 * Check if the NVMf DOCA DPA communication channel is stopped
 *
 * @comch [in]: The NVMf DOCA DPA communication channel to check
 * @return: true in case context is idle and false otherwise
 */
static bool nvmf_doca_dpa_comch_idle(struct nvmf_doca_dpa_comch *comch)
{
	return is_ctx_idle(doca_comch_consumer_as_ctx(comch->recv.consumer)) &&
	       is_ctx_idle(doca_comch_producer_as_ctx(comch->send.producer));
}

/*
 * Continue async flow of stopping the NVMf DOCA DPA communication channel
 *
 * @comch [in]: The NVMf DOCA DPA communication channel to stop
 */
static void nvmf_doca_dpa_comch_stop_continue(struct nvmf_doca_dpa_comch *comch)
{
	if (!nvmf_doca_dpa_comch_idle(comch)) {
		return;
	}

	struct nvmf_doca_io *io = SPDK_CONTAINEROF(comch, struct nvmf_doca_io, comch);
	nvmf_doca_io_stop_continue(io);
}

/*
 * Stop the NVMf DOCA DPA communication channel
 *
 * This operation is async, once operation completes then nvmf_doca_dpa_comch_stop_continue() will be invoked
 *
 * @comch [in]: The NVMf DOCA DPA communication channel to stop
 */
static void nvmf_doca_dpa_comch_stop(struct nvmf_doca_dpa_comch *comch)
{
	nvmf_doca_dpa_msgq_stop(&comch->recv);
	nvmf_doca_dpa_msgq_stop(&comch->send);
}

/*
 * Destroy NVMf DOCA DPA communication channel
 *
 * @comch [in]: The communication channel to destroy
 */
static void nvmf_doca_dpa_comch_destroy(struct nvmf_doca_dpa_comch *comch)
{
	doca_error_t result;

	nvmf_doca_dpa_msgq_destroy(&comch->recv);
	nvmf_doca_dpa_msgq_destroy(&comch->send);

	if (comch->producer_comp != NULL) {
		result = doca_dpa_completion_stop(comch->producer_comp);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA comch: Failed to stop producer completion - %s",
				     doca_error_get_name(result));
		result = doca_dpa_completion_destroy(comch->producer_comp);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR(
				"Failed to destroy NVMf DOCA DPA comch: Failed to destroy producer completion - %s",
				doca_error_get_name(result));
		comch->producer_comp = NULL;
	}

	if (comch->consumer_comp != NULL) {
		result = doca_comch_consumer_completion_stop(comch->consumer_comp);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA comch: Failed to stop consumer completion - %s",
				     doca_error_get_name(result));
		result = doca_comch_consumer_completion_destroy(comch->consumer_comp);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR(
				"Failed to destroy NVMf DOCA DPA comch: Failed to destroy consumer completion - %s",
				doca_error_get_name(result));
		comch->consumer_comp = NULL;
	}
}

struct nvmf_doca_queue_create_attr {
	struct doca_pe *pe;		     /**< Progress engine to be used by DMA context */
	struct doca_dev *dev;		     /**< A doca device representing the emulation manager */
	struct doca_mmap *remote_queue_mmap; /**< mmap granting access to the Host queue memory */
	uintptr_t remote_queue_address;	     /**< I/O address of the queue on the Host */
	uint16_t queue_depth;		     /**< The log of the queue number of elements */
	uint8_t element_size;		     /**< Size in bytes of each element in the queue */
	bool is_read_from_remote; /**< true in case queue will be used to read memory from Host to local buffers */
	doca_dma_task_memcpy_completion_cb_t success_cb; /**< Callback invoked upon DMA of each element */
	doca_dma_task_memcpy_completion_cb_t error_cb;	 /**< Callback invoked upon DMA failure of each element */
	doca_ctx_state_changed_callback_t dma_state_changed_cb; /**< Callback invoked upon DMA state change */
	void *dma_user_data; /**< User data to be provided in the callbacks as the ctx_user_data argument */
};

/*
 * Create NVMf DOCA queue for read/write data from Host SQ/CQ
 *
 * @attr [in]: The queue attributes
 * @queue [out]: The newly created queue
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_queue_create(const struct nvmf_doca_queue_create_attr *attr,
					   struct nvmf_doca_queue *queue)
{
	struct doca_ctx *dma_ctx;
	doca_error_t result;

	memset(queue, 0, sizeof(*queue));

	uint32_t num_elements = attr->queue_depth;

	result = doca_buf_inventory_create(num_elements * 2, &queue->inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to create inventory - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_buf_inventory_start(queue->inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to start inventory - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_dma_create(attr->dev, &queue->dma);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to create DMA context - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_dma_task_memcpy_set_conf(queue->dma, attr->success_cb, attr->error_cb, num_elements);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to set DMA context task consigurations - %s",
			     doca_error_get_name(result));
		return result;
	}
	dma_ctx = doca_dma_as_ctx(queue->dma);
	result = doca_pe_connect_ctx(attr->pe, dma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to connect DMA context to progress engine - %s",
			     doca_error_get_name(result));
		return result;
	}
	union doca_data ctx_user_data;
	ctx_user_data.ptr = attr->dma_user_data;
	result = doca_ctx_set_user_data(dma_ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to set DMA context user data - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_ctx_set_state_changed_cb(dma_ctx, attr->dma_state_changed_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to set DMA context state changed callback - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_ctx_start(dma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to start DMA context - %s",
			     doca_error_get_name(result));
		return result;
	}

	uint32_t queue_size = num_elements * attr->element_size;
	queue->local_queue_address = calloc(1, queue_size);
	if (queue->local_queue_address == NULL) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to allocate memory for local queue");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_create(&queue->local_queue_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to create local queue mmap - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_set_memrange(queue->local_queue_mmap, queue->local_queue_address, queue_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to create local queue mmap - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_set_permissions(queue->local_queue_mmap,
					   attr->is_read_from_remote ? DOCA_ACCESS_FLAG_LOCAL_READ_WRITE :
								       DOCA_ACCESS_FLAG_LOCAL_READ_ONLY);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to set local queue mmap permissions - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_add_dev(queue->local_queue_mmap, attr->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to add device to local queue mmap - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_start(queue->local_queue_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to start local queue mmap - %s",
			     doca_error_get_name(result));
		return result;
	}

	queue->elements = calloc(num_elements, sizeof(*queue->elements));
	if (queue->elements == NULL) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to allocate memory for queue element tasks");
		return DOCA_ERROR_NO_MEMORY;
	}

	for (uint32_t idx = 0; idx < num_elements; idx++) {
		void *local_element_address = (uint8_t *)queue->local_queue_address + idx * attr->element_size;
		struct doca_buf *local_element_buf;
		result = doca_buf_inventory_buf_get_by_addr(queue->inventory,
							    queue->local_queue_mmap,
							    local_element_address,
							    attr->element_size,
							    &local_element_buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create NVMf DOCA Queue: Failed to get local buffer from inventory - %s",
				     doca_error_get_name(result));
			return result;
		}

		void *remote_element_address = (uint8_t *)attr->remote_queue_address + idx * attr->element_size;
		struct doca_buf *remote_element_buf;
		result = doca_buf_inventory_buf_get_by_addr(queue->inventory,
							    attr->remote_queue_mmap,
							    remote_element_address,
							    attr->element_size,
							    &remote_element_buf);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA Queue: Failed to get remote buffer from inventory - %s",
				doca_error_get_name(result));
			doca_buf_dec_refcount(local_element_buf, NULL);
			return result;
		}

		struct doca_buf *src_buf = attr->is_read_from_remote ? remote_element_buf : local_element_buf;
		struct doca_buf *dst_buf = attr->is_read_from_remote ? local_element_buf : remote_element_buf;

		result = doca_buf_set_data_len(src_buf, attr->element_size);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA Queue: Failed to set data length of source buffer for element at index %u - %s",
				idx,
				doca_error_get_name(result));
			doca_buf_dec_refcount(local_element_buf, NULL);
			doca_buf_dec_refcount(remote_element_buf, NULL);
			return result;
		}

		union doca_data task_data;
		task_data.u64 = idx;
		result =
			doca_dma_task_memcpy_alloc_init(queue->dma, src_buf, dst_buf, task_data, &queue->elements[idx]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to create NVMf DOCA Queue: Failed to allocate DMA task for element at index %u - %s",
				idx,
				doca_error_get_name(result));
			doca_buf_dec_refcount(local_element_buf, NULL);
			doca_buf_dec_refcount(remote_element_buf, NULL);
			return result;
		}
	}
	queue->num_elements = num_elements;

	return DOCA_SUCCESS;
}

/*
 * Free all elements of the NVMf DOCA queue
 *
 * As long as the DMA tasks are not free then the DMA context will not move to idle state
 *
 * @queue [in]: The NVMf DOCA queue
 */
static void nvmf_doca_queue_free_elements(struct nvmf_doca_queue *queue)
{
	doca_error_t result;

	if (queue->elements != NULL) {
		for (uint32_t idx = 0; idx < queue->num_elements; idx++) {
			struct doca_dma_task_memcpy *element = queue->elements[idx];
			if (element == NULL)
				continue;

			struct doca_buf *src_buf = (struct doca_buf *)doca_dma_task_memcpy_get_src(element);
			struct doca_buf *dst_buf = doca_dma_task_memcpy_get_dst(element);

			doca_task_free(doca_dma_task_memcpy_as_task(element));
			result = doca_buf_dec_refcount(src_buf, NULL);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR(
					"Failed to destroy NVMf DOCA Queue: Failed to free source buffer at index %u - %s",
					idx,
					doca_error_get_name(result));
			result = doca_buf_dec_refcount(dst_buf, NULL);
			if (result != DOCA_SUCCESS)
				DOCA_LOG_ERR(
					"Failed to destroy NVMf DOCA Queue: Failed to free destination buffer at index %u - %s",
					idx,
					doca_error_get_name(result));
		}
		queue->num_elements = 0;

		free(queue->elements);
		queue->elements = NULL;
	}
}

/*
 * Check if a DOCA context state is running
 *
 * @ctx [in]: The DOCA context to check
 * @return: true in case context is running and false otherwise
 */
static bool is_ctx_running(struct doca_ctx *ctx)
{
	enum doca_ctx_states ctx_state;
	doca_error_t result;

	result = doca_ctx_get_state(ctx, &ctx_state);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to check if context is idle: Failed to get context state - %s",
			     doca_error_get_name(result));
		return false;
	}

	return ctx_state == DOCA_CTX_STATE_RUNNING;
}

/*
 * Stop the NVMf DOCA queue
 *
 * This operation is async, once operation completes then nvmf_doca_cq_queue_dma_state_changed_cb() or
 * nvmf_doca_sq_queue_dma_state_changed_cb() will be invoked. Depending on owner of the queue
 *
 * @queue [in]: The NVMf DOCA queue to stop
 */
static void nvmf_doca_queue_stop(struct nvmf_doca_queue *queue)
{
	doca_error_t result;
	size_t num_inflight;

	if (!is_ctx_running(doca_dma_as_ctx(queue->dma))) {
		return;
	}

	result = doca_ctx_stop(doca_dma_as_ctx(queue->dma));
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA Queue: Failed to stop DMA context - %s",
			     doca_error_get_name(result));
	}

	result = doca_ctx_get_num_inflight_tasks(doca_dma_as_ctx(queue->dma), &num_inflight);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA Queue: Failed to get number of inflight DMA tasks - %s",
			     doca_error_get_name(result));
		return;
	}
	if (num_inflight == 0) {
		nvmf_doca_queue_free_elements(queue);
	}
}

/*
 * Destroy NVMf DOCA queue
 *
 * @queue [in]: The queue to destroy
 */
static void nvmf_doca_queue_destroy(struct nvmf_doca_queue *queue)
{
	doca_error_t result;

	nvmf_doca_queue_free_elements(queue);

	if (queue->local_queue_mmap != NULL) {
		result = doca_mmap_destroy(queue->local_queue_mmap);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA Queue: Failed to destroy local queue mmap %s",
				     doca_error_get_name(result));
		queue->local_queue_mmap = NULL;
	}

	if (queue->local_queue_address != NULL) {
		free(queue->local_queue_address);
		queue->local_queue_address = NULL;
	}

	if (queue->dma != NULL) {
		result = doca_ctx_stop(doca_dma_as_ctx(queue->dma));
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA Queue: Failed to stop DMA context %s",
				     doca_error_get_name(result));
		result = doca_dma_destroy(queue->dma);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA Queue: Failed to destroy DMA context %s",
				     doca_error_get_name(result));
		queue->dma = NULL;
	}

	if (queue->inventory != NULL) {
		result = doca_buf_inventory_destroy(queue->inventory);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA Queue: Failed to destroy inventory %s",
				     doca_error_get_name(result));
		queue->inventory = NULL;
	}
}

struct nvmf_doca_cq_create_attr {
	struct doca_pe *pe;			       /**< Progress engine to be used by DMA context */
	struct doca_dev *dev;			       /**< A doca device representing the emulation manager */
	struct doca_devemu_pci_dev *nvme_dev;	       /**< The emulated NVMe device used for creating the CQ DB */
	uint16_t cq_depth;			       /**< The log of the CQ number of elements */
	struct doca_mmap *host_cq_mmap;		       /**< An mmap granting access to the Host CQ memory */
	uintptr_t host_cq_address;		       /**< I/O address of the CQ on the Host */
	struct doca_devemu_pci_db_completion *db_comp; /**< DB completion context used by DPA to poll for DBs */
	uint32_t cq_id;				       /**< The NVMe CQ ID that is associated with this IO */
	struct nvmf_doca_io *io;		       /**< The IO that the CQ belongs to */
};

/*
 * Update the consumer index of the CQ
 *
 * @cq [in]: The CQ that is being updated
 * @new_ci [in]: The new CQ producer index as provided by the Host
 */
static void nvmf_doca_cq_update_ci(struct nvmf_doca_cq *cq, uint32_t new_ci)
{
	cq->ci = new_ci;
}

/*
 * Raise an MSI-X towards the Host
 *
 * @io [in]: The IO that posted the original CQE
 */
static void nvmf_doca_io_raise_msix(struct nvmf_doca_io *io)
{
	if (io->msix == NULL)
		return;

	struct comch_msg msg = {
		.type = COMCH_MSG_TYPE_RAISE_MSIX,
	};
	nvmf_doca_dpa_msgq_send(&io->comch.send, &msg, sizeof(msg));
}

void nvmf_doca_io_post_cqe(struct nvmf_doca_io *io, const struct nvmf_doca_cqe *cqe, union doca_data user_data)
{
	struct doca_buf *host_cqe_buf;
	const struct doca_buf *dpu_cqe_buf;
	struct nvmf_doca_cqe *dpu_cqe;
	struct nvmf_doca_cq *cq = &io->cq;
	uint32_t cqe_idx = cq->pi % (cq->queue.num_elements);
	struct doca_dma_task_memcpy *cqe_task = cq->queue.elements[cqe_idx];

	host_cqe_buf = doca_dma_task_memcpy_get_dst(cqe_task);
	doca_buf_reset_data_len(host_cqe_buf);

	dpu_cqe_buf = doca_dma_task_memcpy_get_src(cqe_task);
	doca_buf_get_head(dpu_cqe_buf, (void **)&dpu_cqe);
	*dpu_cqe = *cqe;

	/**
	 * Update the phase bit according to the iteration
	 * For every even iteration the phase should be 1, while for odd should be 0
	 */
	uint16_t cqe_phase = !((cq->pi / cq->queue.num_elements) % 2);
	((struct spdk_nvme_cpl *)dpu_cqe)->status.p = cqe_phase;

	struct doca_task *task = doca_dma_task_memcpy_as_task(cqe_task);
	doca_task_set_user_data(task, user_data);
	doca_task_submit(task);

	cq->pi++;
}

/*
 * Callback invoked once a CQE has been successfully posted to Host
 *
 * @task [in]: The DMA memcpy task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the DMA context
 */
static void nvmf_doca_cq_cqe_post_cb(struct doca_dma_task_memcpy *task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
	(void)task;

	struct nvmf_doca_cq *cq = ctx_user_data.ptr;

	nvmf_doca_io_raise_msix(cq->io);
	cq->io->post_cqe_cb(cq, task_user_data);
}

/*
 * Callback invoked once post of CQE fails
 *
 * @task [in]: The DMA memcpy task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the DMA context
 */
static void nvmf_doca_cq_cqe_post_error_cb(struct doca_dma_task_memcpy *task,
					   union doca_data task_user_data,
					   union doca_data ctx_user_data)
{
	(void)task;
	(void)task_user_data;

	size_t num_inflight;
	doca_error_t result;
	struct nvmf_doca_sq *cq = ctx_user_data.ptr;

	result = doca_ctx_get_num_inflight_tasks(doca_dma_as_ctx(cq->queue.dma), &num_inflight);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA CQ: Failed to get number of inflight DMA tasks - %s",
			     doca_error_get_name(result));
		return;
	}
	if (num_inflight == 0) {
		nvmf_doca_queue_free_elements(&cq->queue);
	}
}

/*
 * Callback invoked once state change occurs to the DMA context held by the CQ queue
 *
 * @user_data [in]: The user data associated with the context
 * @ctx [in]: The DMA context
 * @prev_state [in]: The previous state
 * @next_state [in]: The new state
 */
static void nvmf_doca_cq_queue_dma_state_changed_cb(const union doca_data user_data,
						    struct doca_ctx *ctx,
						    enum doca_ctx_states prev_state,
						    enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct nvmf_doca_cq *cq = user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		nvmf_doca_cq_stop_continue(cq);
		break;
	case DOCA_CTX_STATE_STARTING:
	case DOCA_CTX_STATE_RUNNING:
	case DOCA_CTX_STATE_STOPPING:
	default:
		break;
	}
}

/*
 * Create NVMf DOCA CQ for writing CQEs to Host CQ
 *
 * @attr [in]: The CQ attributes
 * @cq [out]: The newly created CQ
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_cq_create(const struct nvmf_doca_cq_create_attr *attr, struct nvmf_doca_cq *cq)
{
	doca_error_t result;

	memset(cq, 0, sizeof(*cq));

	cq->io = attr->io;
	cq->cq_id = attr->cq_id;

	struct nvmf_doca_queue_create_attr queue_attr = {
		.pe = attr->pe,
		.dev = attr->dev,
		.remote_queue_mmap = attr->host_cq_mmap,
		.remote_queue_address = attr->host_cq_address,
		.queue_depth = attr->cq_depth,
		.element_size = sizeof(struct nvmf_doca_cqe),
		.is_read_from_remote = false,
		.success_cb = nvmf_doca_cq_cqe_post_cb,
		.error_cb = nvmf_doca_cq_cqe_post_error_cb,
		.dma_state_changed_cb = nvmf_doca_cq_queue_dma_state_changed_cb,
		.dma_user_data = cq,
	};
	result = nvmf_doca_queue_create(&queue_attr, &cq->queue);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA CQ: Failed to create queue - %s", doca_error_get_name(result));
		return result;
	}

	uint32_t cq_db_id = 2 * attr->cq_id + 1;
	result = doca_devemu_pci_db_create_on_dpa(attr->nvme_dev,
						  attr->db_comp,
						  db_configs[0].region.bar_id,
						  db_configs[0].region.start_address,
						  cq_db_id,
						  /*user_data=*/0,
						  &cq->db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA CQ: Failed to create DB - %s", doca_error_get_name(result));
		return result;
	}
	result = doca_devemu_pci_db_start(cq->db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA CQ: Failed to start DB - %s", doca_error_get_name(result));
		return result;
	}
	return DOCA_SUCCESS;
}

/*
 * Check if the NVMf DOCA CQ is stopped
 *
 * @cq [in]: The NVMf DOCA CQ to check
 * @return: true in case CQ is idle and false otherwise
 */
static bool nvmf_doca_cq_idle(struct nvmf_doca_cq *cq)
{
	return is_ctx_idle(doca_dma_as_ctx(cq->queue.dma));
}

/*
 * Stop the NVMf DOCA CQ
 *
 * This operation is async, once operation completes then nvmf_doca_cq_stop_continue() will be invoked
 *
 * @cq [in]: The NVMf DOCA CQ to stop
 */
static void nvmf_doca_cq_stop(struct nvmf_doca_cq *cq)
{
	doca_error_t result;

	result = doca_devemu_pci_db_stop(cq->db);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA CQ: Failed to stop DB - %s", doca_error_get_name(result));
	}

	nvmf_doca_queue_stop(&cq->queue);
}

/*
 * Continue async flow of stopping the NVMf DOCA CQ
 *
 * @cq [in]: The NVMf DOCA CQ to stop
 */
static void nvmf_doca_cq_stop_continue(struct nvmf_doca_cq *cq)
{
	/* If reached here means DMA context has been successfully stopped now we can continue stopping the IO */
	nvmf_doca_io_stop_continue(cq->io);
}

/*
 * Destroy NVMf DOCA CQ
 *
 * @cq [in]: The CQ to destroy
 */
static void nvmf_doca_cq_destroy(struct nvmf_doca_cq *cq)
{
	doca_error_t result;

	if (cq->db != NULL) {
		result = doca_devemu_pci_db_stop(cq->db);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA CQ: Failed to stop DB - %s",
				     doca_error_get_name(result));
		result = doca_devemu_pci_db_destroy(cq->db);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA CQ: Failed to destroy DB - %s",
				     doca_error_get_name(result));
		cq->db = NULL;
	}

	nvmf_doca_queue_destroy(&cq->queue);
}

struct nvmf_doca_dpa_thread_create_attr {
	struct doca_dpa *dpa;		 /**< DOCA DPA used for creating the DPA thread */
	doca_dpa_func_t *thread_handler; /**< Handler function that the thread will execute */
	size_t thread_arg_size;		 /**< Size in bytes of argument that will be used in the thread */
};

/*
 * Create NVMf DOCA DPA thread
 *
 * @attr [in]: The thread attributes
 * @dpa_thread [out]: The newly created DPA thread
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_dpa_thread_create(const struct nvmf_doca_dpa_thread_create_attr *attr,
						struct nvmf_doca_dpa_thread *dpa_thread)
{
	doca_error_t result;

	memset(dpa_thread, 0, sizeof(*dpa_thread));

	dpa_thread->dpa = attr->dpa;

	result = doca_dpa_mem_alloc(attr->dpa, attr->thread_arg_size, &dpa_thread->arg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA thread: Failed to allocate thread argument memory - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_dpa_thread_create(attr->dpa, &dpa_thread->thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA thread: Failed to create DPA thread - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_dpa_thread_set_func_arg(dpa_thread->thread, attr->thread_handler, dpa_thread->arg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA thread: Failed to set DPA thread function - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_dpa_thread_start(dpa_thread->thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA thread: Failed to start DPA thread - %s",
			     doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Copy arguments and run the NVMf DOCA DPA thread
 *
 * @dpa_thread [in]: The DPA thread to run
 * @arg [in]: Pointer to arguments that will be copied to DPA as the DPA thread arguments
 * @arg_size [in]: The size in bytes of the thread arguments
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_dpa_thread_run(struct nvmf_doca_dpa_thread *dpa_thread, void *arg, size_t arg_size)
{
	doca_error_t result;

	result = doca_dpa_h2d_memcpy(dpa_thread->dpa, dpa_thread->arg, arg, arg_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to run NVMf DOCA DPA thread: Failed to update DPA thread argument - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_dpa_thread_run(dpa_thread->thread);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to run NVMf DOCA DPA thread: Failed to run DPA thread - %s",
			     doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Destroy NVMf DOCA DPA thread
 *
 * @dpa_thread [in]: The DPA thread to destroy
 */
static void nvmf_doca_dpa_thread_destroy(struct nvmf_doca_dpa_thread *dpa_thread)
{
	doca_error_t result;

	if (dpa_thread->thread != NULL) {
		result = doca_dpa_thread_stop(dpa_thread->thread);
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA thread: Failed to stop DPA thread - %s",
				     doca_error_get_name(result));
		result = doca_dpa_thread_destroy(dpa_thread->thread);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA thread: Failed to destroy DPA thread - %s",
				     doca_error_get_name(result));
		dpa_thread->thread = NULL;
	}

	if (dpa_thread->arg != 0) {
		result = doca_dpa_mem_free(dpa_thread->dpa, dpa_thread->arg);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DPA thread: Failed to free DPA thread argument - %s",
				     doca_error_get_name(result));
		dpa_thread->arg = 0;
	}
}

/*
 * Fills the DPA thread argument with the relevant DPA handles to be later copied to the DPA thread
 *
 * @io [in]: The NVMf DOCA IO that contains the DPA resources
 * @arg [out]: The returned thread argument that was filled
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_io_fill_thread_arg(struct nvmf_doca_io *io, struct io_thread_arg *arg)
{
	doca_error_t result;

	doca_dpa_dev_comch_consumer_completion_t dpa_consumer_comp;
	doca_dpa_dev_completion_t dpa_producer_comp;
	doca_dpa_dev_comch_producer_t dpa_producer;
	doca_dpa_dev_comch_consumer_t dpa_consumer;
	doca_dpa_dev_devemu_pci_db_completion_t dpa_db_comp;
	doca_dpa_dev_devemu_pci_msix_t dpa_msix;

	result = doca_comch_consumer_completion_get_dpa_handle(io->comch.consumer_comp, &dpa_consumer_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to fill NVMf DOCA IO thread argument: Failed to get consumer completion context DPA handle - %s",
			doca_error_get_name(result));
		return result;
	}
	result = doca_dpa_completion_get_dpa_handle(io->comch.producer_comp, &dpa_producer_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to fill NVMf DOCA IO thread argument: Failed to get producer completion context DPA handle - %s",
			doca_error_get_name(result));
		return result;
	}
	result = doca_comch_consumer_get_dpa_handle(io->comch.send.consumer, &dpa_consumer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill NVMf DOCA IO thread argument: Failed to get consumer DPA handle - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_comch_producer_get_dpa_handle(io->comch.recv.producer, &dpa_producer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to fill NVMf DOCA IO thread argument: Failed to get producer DPA handle - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_devemu_pci_db_completion_get_dpa_handle(io->db_comp, &dpa_db_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to fill NVMf DOCA IO thread argument: Failed to get DB completion context DPA handle - %s",
			doca_error_get_name(result));
		return result;
	}

	dpa_msix = 0;
	if (io->msix != NULL) {
		result = doca_devemu_pci_msix_get_dpa_handle(io->msix, &dpa_msix);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to fill NVMf DOCA IO thread argument: Failed to get MSI-X DPA handle - %s",
				     doca_error_get_name(result));
			return result;
		}
	}

	*arg = (struct io_thread_arg){
		.dpa_consumer_comp = dpa_consumer_comp,
		.dpa_producer_comp = dpa_producer_comp,
		.dpa_producer = dpa_producer,
		.dpa_consumer = dpa_consumer,
		.dpa_db_comp = dpa_db_comp,
		.dpa_msix = dpa_msix,
	};

	return DOCA_SUCCESS;
}

/*
 * Initialize and run the NVMf DOCA DPA thread
 *
 * @io [in]: The NVMf DOCA IO that contains the DPA resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_io_run_dpa_thread(struct nvmf_doca_io *io)
{
	struct io_thread_arg arg;
	doca_error_t result;

	result = nvmf_doca_io_fill_thread_arg(io, &arg);
	if (result != DOCA_SUCCESS)
		return result;

	doca_dpa_dev_devemu_pci_db_t dpa_cq_db;
	result = doca_devemu_pci_db_get_dpa_handle(io->cq.db, &dpa_cq_db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to run IO DPA thread: Failed to get CQ DB DPA handle");
		return result;
	}

	uint64_t rpc_ret;
	result = doca_dpa_rpc(io->dpa_thread.dpa,
			      io_thread_init_rpc,
			      &rpc_ret,
			      arg.dpa_db_comp,
			      dpa_cq_db,
			      arg.dpa_consumer);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to run IO DPA thread: Failed to issue initialize thread RPC - %s",
			     doca_error_get_name(result));
		return result;
	}
	if (rpc_ret != RPC_RETURN_STATUS_SUCCESS) {
		DOCA_LOG_ERR("Failed to run IO DPA thread: The initialize thread RPC has failed");
		return result;
	}

	result = nvmf_doca_dpa_thread_run(&io->dpa_thread, &arg, sizeof(arg));
	if (result != DOCA_SUCCESS)
		return result;

	return DOCA_SUCCESS;
}

doca_error_t nvmf_doca_io_create(const struct nvmf_doca_io_create_attr *attr, struct nvmf_doca_io *io)
{
	doca_error_t result;

	struct nvmf_doca_dpa_thread_create_attr dpa_thread_attr = {
		.dpa = attr->dpa,
		.thread_handler = io_thread,
		.thread_arg_size = sizeof(struct io_thread_arg),
	};
	result = nvmf_doca_dpa_thread_create(&dpa_thread_attr, &io->dpa_thread);
	if (result != DOCA_SUCCESS) {
		nvmf_doca_io_destroy(io);
		return result;
	}

	struct nvmf_doca_dpa_comch_create_attr comch_attr = {
		.dev = attr->dev,
		.dpa = attr->dpa,
		.dpa_thread = io->dpa_thread.thread,
		.max_num_msg = MAX_NUM_COMCH_MSGS,
		.pe = attr->pe,
		.ctx_user_data = io,
	};
	result = nvmf_doca_dpa_comch_create(&comch_attr, &io->comch);
	if (result != DOCA_SUCCESS) {
		nvmf_doca_io_destroy(io);
		return result;
	}

	result = doca_devemu_pci_db_completion_create(io->dpa_thread.thread, &io->db_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA IO: Failed to create DB completion context - %s",
			     doca_error_get_name(result));
		nvmf_doca_io_destroy(io);
		return result;
	}
	result = doca_devemu_pci_db_completion_set_max_num_dbs(io->db_comp, attr->max_num_sq + 1);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create NVMf DOCA DPA IO: Failed to set DB completion context max number of doorbells - %s",
			doca_error_get_name(result));
		nvmf_doca_io_destroy(io);
		return result;
	}
	result = doca_devemu_pci_db_completion_start(io->db_comp);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DPA IO: Failed to start DB completion context - %s",
			     doca_error_get_name(result));
		nvmf_doca_io_destroy(io);
		return result;
	}

	if (attr->enable_msix) {
		result = doca_devemu_pci_msix_create_on_dpa(attr->nvme_dev,
							    msix_table_configs[0].bar_id,
							    msix_table_configs[0].start_address,
							    attr->msix_idx,
							    /*user_data=*/attr->msix_idx,
							    &io->msix);
		if (result != DOCA_SUCCESS) {
			nvmf_doca_io_destroy(io);
			return result;
		}
	}

	struct nvmf_doca_cq_create_attr cq_attr = {
		.pe = attr->pe,
		.dev = attr->dev,
		.nvme_dev = attr->nvme_dev,
		.cq_depth = attr->cq_depth,
		.host_cq_mmap = attr->host_cq_mmap,
		.host_cq_address = attr->host_cq_address,
		.db_comp = io->db_comp,
		.cq_id = attr->cq_id,
		.io = io,
	};
	result = nvmf_doca_cq_create(&cq_attr, &io->cq);
	if (result != DOCA_SUCCESS) {
		nvmf_doca_io_destroy(io);
		return result;
	}

	result = nvmf_doca_io_run_dpa_thread(io);
	if (result != DOCA_SUCCESS) {
		nvmf_doca_io_destroy(io);
		return result;
	}

	TAILQ_INIT(&io->sq_list);
	io->post_cqe_cb = attr->post_cqe_cb;
	io->fetch_sqe_cb = attr->fetch_sqe_cb;
	io->copy_data_cb = attr->copy_data_cb;
	io->stop_sq_cb = attr->stop_sq_cb;
	io->stop_io_cb = attr->stop_io_cb;

	return DOCA_SUCCESS;
}

void nvmf_doca_io_destroy(struct nvmf_doca_io *io)
{
	doca_error_t result;

	nvmf_doca_cq_destroy(&io->cq);

	if (io->msix != NULL) {
		result = doca_devemu_pci_msix_destroy(io->msix);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA IO: Failed to destroy MSI-X");
		io->msix = NULL;
	}

	if (io->db_comp != NULL) {
		result = doca_devemu_pci_db_completion_stop(io->db_comp);
		if (result != DOCA_SUCCESS && result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA IO: Failed to stop DB completion context");
		result = doca_devemu_pci_db_completion_destroy(io->db_comp);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA IO: Failed to destroy DB completion context");
		io->db_comp = NULL;
	}

	nvmf_doca_dpa_comch_destroy(&io->comch);

	nvmf_doca_dpa_thread_destroy(&io->dpa_thread);
}

void nvmf_doca_io_stop(struct nvmf_doca_io *io)
{
	if (!TAILQ_EMPTY(&io->sq_list)) {
		DOCA_LOG_ERR("Failed to stop IO: Not all SQs have been removed");
		return;
	}

	if (!nvmf_doca_cq_idle(&io->cq)) {
		nvmf_doca_cq_stop(&io->cq);
		return;
	}

	nvmf_doca_io_stop_continue(io);
}

/*
 * Continue async flow of stopping the NVMf DOCA IO
 *
 * @io [in]: The NVMf DOCA IO to stop
 */
static void nvmf_doca_io_stop_continue(struct nvmf_doca_io *io)
{
	if (!nvmf_doca_cq_idle(&io->cq)) {
		return;
	}

	if (!nvmf_doca_dpa_comch_idle(&io->comch)) {
		nvmf_doca_dpa_comch_stop(&io->comch);
		return;
	}

	io->stop_io_cb(io);
}

struct nvmf_doca_dma_pool_create_attr {
	struct doca_pe *pe;				 /**< Progress engine to be used by DMA context */
	struct doca_dev *dev;				 /**< A doca device representing the emulation manager */
	uint32_t max_dma_operations;			 /**< The maximal number of DMA copy operations */
	uint32_t max_dma_operation_size;		 /**< The maximal size in bytes of the DMA copy operation */
	struct doca_mmap *host_data_mmap;		 /**< An mmap granting access to the Host Data memory */
	doca_dma_task_memcpy_completion_cb_t success_cb; /**< Callback invoked upon DMA of data buffer */
	doca_dma_task_memcpy_completion_cb_t error_cb;	 /**< Callback invoked upon DMA failure of data buffer */
	doca_ctx_state_changed_callback_t dma_state_changed_cb; /**< Callback invoked upon state change of DMA */
	void *dma_user_data; /**< User data to be provided in the callbacks as the ctx_user_data argument */
};

struct doca_buf *nvmf_doca_sq_get_dpu_buffer(struct nvmf_doca_sq *sq)
{
	struct doca_buf *buf;

	doca_buf_pool_buf_alloc(sq->dma_pool.local_data_pool, &buf);

	return buf;
}

struct doca_buf *nvmf_doca_sq_get_host_buffer(struct nvmf_doca_sq *sq, uintptr_t host_io_address)
{
	struct doca_buf *buf;

	doca_buf_inventory_buf_get_by_addr(sq->dma_pool.host_data_inventory,
					   sq->dma_pool.host_data_mmap,
					   (void *)host_io_address,
					   DMA_POOL_DATA_BUFFER_SIZE,
					   &buf);

	return buf;
}

void nvmf_doca_sq_copy_data(struct nvmf_doca_sq *sq,
			    struct doca_buf *dst_buffer,
			    struct doca_buf *src_buffer,
			    size_t length,
			    union doca_data user_data)
{
	struct doca_dma_task_memcpy *dma_task;

	doca_buf_reset_data_len(dst_buffer);
	doca_buf_set_data_len(src_buffer, length);

	doca_dma_task_memcpy_alloc_init(sq->dma_pool.dma, src_buffer, dst_buffer, user_data, &dma_task);
	doca_task_submit(doca_dma_task_memcpy_as_task(dma_task));
}

/*
 * Callback invoked once data has been successfully copied from/to Host
 *
 * @task [in]: The DMA memcpy task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the DMA context
 */
static void nvmf_doca_dma_pool_copy_cb(struct doca_dma_task_memcpy *task,
				       union doca_data task_user_data,
				       union doca_data ctx_user_data)
{
	struct nvmf_doca_sq *sq = ctx_user_data.ptr;
	struct doca_buf *dst = doca_dma_task_memcpy_get_dst(task);
	struct doca_buf *src = (struct doca_buf *)doca_dma_task_memcpy_get_src(task);

	doca_task_free(doca_dma_task_memcpy_as_task(task));

	sq->io->copy_data_cb(sq, dst, src, task_user_data);
}

/*
 * Callback invoked once copy of data fails
 *
 * @task [in]: The DMA memcpy task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the DMA context
 */
static void nvmf_doca_dma_pool_copy_error_cb(struct doca_dma_task_memcpy *task,
					     union doca_data task_user_data,
					     union doca_data ctx_user_data)
{
	(void)task_user_data;
	(void)ctx_user_data;

	doca_task_free(doca_dma_task_memcpy_as_task(task));
}

/*
 * Callback invoked once state change occurs to the DMA context held by the DMA pool
 *
 * @user_data [in]: The user data associated with the context
 * @ctx [in]: The DMA context
 * @prev_state [in]: The previous state
 * @next_state [in]: The new state
 */
static void nvmf_doca_dma_pool_dma_state_changed_cb(const union doca_data user_data,
						    struct doca_ctx *ctx,
						    enum doca_ctx_states prev_state,
						    enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct nvmf_doca_sq *sq = user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		nvmf_doca_sq_stop_continue(sq);
		break;
	case DOCA_CTX_STATE_STARTING:
	case DOCA_CTX_STATE_RUNNING:
	case DOCA_CTX_STATE_STOPPING:
	default:
		break;
	}
}

/*
 * Create NVMf DOCA DMA pool for copying data between Host and DPU
 *
 * @attr [in]: The DMA pool attributes
 * @dma_pool [out]: The newly created DMA pool
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_dma_pool_create(const struct nvmf_doca_dma_pool_create_attr *attr,
					      struct nvmf_doca_dma_pool *dma_pool)
{
	doca_error_t result;

	memset(dma_pool, 0, sizeof(*dma_pool));

	uint32_t local_data_memory_size = attr->max_dma_operations * attr->max_dma_operation_size;
	dma_pool->local_data_memory = spdk_dma_zmalloc(local_data_memory_size, CACHELINE_SIZE_BYTES, NULL);
	if (dma_pool->local_data_memory == NULL) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to allocate memory for local data");
		return DOCA_ERROR_NO_MEMORY;
	}

	result = doca_mmap_create(&dma_pool->local_data_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to create local data mmap - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_set_memrange(dma_pool->local_data_mmap, dma_pool->local_data_memory, local_data_memory_size);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to create local data mmap - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_set_permissions(dma_pool->local_data_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to set local data mmap permissions - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_add_dev(dma_pool->local_data_mmap, attr->dev);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to add device to local dma mmap - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_mmap_start(dma_pool->local_data_mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to start local dma mmap - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_buf_pool_create(attr->max_dma_operations,
				      attr->max_dma_operation_size,
				      dma_pool->local_data_mmap,
				      &dma_pool->local_data_pool);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to create local data pool - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_buf_pool_start(dma_pool->local_data_pool);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to start local data pool - %s",
			     doca_error_get_name(result));
		return result;
	}

	result = doca_buf_inventory_create(attr->max_dma_operations, &dma_pool->host_data_inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to create Host data inventory - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_buf_inventory_start(dma_pool->host_data_inventory);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to start Host data inventory - %s",
			     doca_error_get_name(result));
		return result;
	}
	dma_pool->host_data_mmap = attr->host_data_mmap;

	result = doca_dma_create(attr->dev, &dma_pool->dma);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to create DMA context - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_dma_task_memcpy_set_conf(dma_pool->dma,
					       attr->success_cb,
					       attr->error_cb,
					       attr->max_dma_operations);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to set DMA context task consigurations - %s",
			     doca_error_get_name(result));
		return result;
	}
	struct doca_ctx *dma_ctx = doca_dma_as_ctx(dma_pool->dma);
	result = doca_pe_connect_ctx(attr->pe, dma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create NVMf DOCA DMA pool: Failed to connect DMA context to progress engine - %s",
			doca_error_get_name(result));
		return result;
	}
	union doca_data ctx_user_data;
	ctx_user_data.ptr = attr->dma_user_data;
	result = doca_ctx_set_user_data(dma_ctx, ctx_user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to set DMA context user data - %s",
			     doca_error_get_name(result));
		return result;
	}
	result = doca_ctx_set_state_changed_cb(dma_ctx, attr->dma_state_changed_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR(
			"Failed to create NVMf DOCA DMA pool: Failed to set DMA context state changed callback - %s",
			doca_error_get_name(result));
		return result;
	}
	result = doca_ctx_start(dma_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA DMA pool: Failed to start DMA context - %s",
			     doca_error_get_name(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Stop the NVMf DOCA DMA pool
 *
 * This operation is async, once operation completes then nvmf_doca_dma_pool_dma_state_changed_cb() will be invoked
 *
 * @dma_pool [in]: The NVMf DOCA DMA pool to stop
 */
static void nvmf_doca_dma_pool_stop(struct nvmf_doca_dma_pool *dma_pool)
{
	doca_error_t result;

	if (!is_ctx_running(doca_dma_as_ctx(dma_pool->dma))) {
		return;
	}

	result = doca_ctx_stop(doca_dma_as_ctx(dma_pool->dma));
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_IN_PROGRESS && result != DOCA_ERROR_BAD_STATE) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA DMA pool: Failed to stop context - %s",
			     doca_error_get_name(result));
	}
}

/*
 * Destroy NVMf DOCA DMA pool
 *
 * @dma_pool [in]: The DMA pool to destroy
 */
static void nvmf_doca_dma_pool_destroy(struct nvmf_doca_dma_pool *dma_pool)
{
	doca_error_t result;

	if (dma_pool->dma != NULL) {
		result = doca_ctx_stop(doca_dma_as_ctx(dma_pool->dma));
		if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DMA pool: Failed to stop DMA context %s",
				     doca_error_get_name(result));
		result = doca_dma_destroy(dma_pool->dma);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DMA pool: Failed to destroy DMA context %s",
				     doca_error_get_name(result));
		dma_pool->dma = NULL;
	}

	dma_pool->host_data_mmap = NULL;
	if (dma_pool->host_data_inventory != NULL) {
		result = doca_buf_inventory_destroy(dma_pool->host_data_inventory);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DMA pool: Failed to destroy host data inventory %s",
				     doca_error_get_name(result));
		dma_pool->host_data_inventory = NULL;
	}

	if (dma_pool->local_data_pool != NULL) {
		result = doca_buf_pool_destroy(dma_pool->local_data_pool);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DMA pool: Failed to destroy local data pool %s",
				     doca_error_get_name(result));
		dma_pool->local_data_pool = NULL;
	}

	if (dma_pool->local_data_mmap != NULL) {
		result = doca_mmap_destroy(dma_pool->local_data_mmap);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA DMA pool: Failed to destroy local data mmap %s",
				     doca_error_get_name(result));
		dma_pool->local_data_mmap = NULL;
	}

	if (dma_pool->local_data_memory != NULL) {
		spdk_dma_free(dma_pool->local_data_memory);
		dma_pool->local_data_memory = NULL;
	}
}

/*
 * Update the producer index of the SQ
 *
 * This may cause read operations of SQEs
 *
 * @sq [in]: The SQ that is being updated
 * @new_pi [in]: The new SQ producer index as provided by the Host
 */
static void nvmf_doca_sq_update_pi(struct nvmf_doca_sq *sq, uint32_t new_pi)
{
	struct doca_dma_task_memcpy *sqe_task;
	struct doca_buf *dst_buffer;
	uint16_t sqe_idx;
	uint16_t pi = sq->pi;
	uint16_t num_sqes;
	if (new_pi >= pi) {
		num_sqes = (new_pi - pi);
	} else {
		num_sqes = ((sq->queue.num_elements - pi) + new_pi);
	}

	for (uint16_t sqe_count = 0; sqe_count < num_sqes; sqe_count++) {
		sqe_idx = (pi + sqe_count) % sq->queue.num_elements;
		sqe_task = sq->queue.elements[sqe_idx];

		dst_buffer = doca_dma_task_memcpy_get_dst(sqe_task);
		doca_buf_reset_data_len(dst_buffer);
		if (sqe_count == (num_sqes - 1)) {
			doca_task_submit_ex(doca_dma_task_memcpy_as_task(sqe_task), DOCA_TASK_SUBMIT_FLAG_FLUSH);
		} else {
			doca_task_submit_ex(doca_dma_task_memcpy_as_task(sqe_task),
					    DOCA_TASK_SUBMIT_FLAG_OPTIMIZE_REPORTS);
		}
	}

	sq->pi = new_pi;
}

/*
 * Method invoked once Host DB message is received from DPA
 *
 * @io [in]: The IO used to receive the message
 * @msg [in]: The Host DB message that was received from DPA
 */
static void nvmf_doca_io_handle_host_db_msg(struct nvmf_doca_io *io, const struct comch_msg *msg)
{
	struct nvmf_doca_sq *sq = (struct nvmf_doca_sq *)msg->host_db_data.db_user_data;
	uint32_t db_value = msg->host_db_data.db_value;

	if (sq == NULL) {
		nvmf_doca_cq_update_ci(&io->cq, db_value);
	} else {
		nvmf_doca_sq_update_pi(sq, db_value);
	}
}

/*
 * Callback invoked once a SQE has been successfully read from Host
 *
 * @task [in]: The DMA memcpy task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the DMA context
 */
static void nvmf_doca_sq_sqe_read_cb(struct doca_dma_task_memcpy *task,
				     union doca_data task_user_data,
				     union doca_data ctx_user_data)
{
	struct doca_buf *sqe_buf;
	struct nvmf_doca_sqe *sqe;
	struct nvmf_doca_sq *sq = ctx_user_data.ptr;
	uint16_t sqe_idx = task_user_data.u64;

	sqe_buf = doca_dma_task_memcpy_get_dst(task);
	doca_buf_get_data(sqe_buf, (void **)&sqe);

	sq->io->fetch_sqe_cb(sq, sqe, sqe_idx);
}

/*
 * Callback invoked once read of SQE fails
 *
 * @task [in]: The DMA memcpy task
 * @task_user_data [in]: User data that was previously provided with the task
 * @ctx_user_data [in]: User data that was previously set for the DMA context
 */
static void nvmf_doca_sq_sqe_read_error_cb(struct doca_dma_task_memcpy *task,
					   union doca_data task_user_data,
					   union doca_data ctx_user_data)
{
	(void)task;
	(void)task_user_data;

	size_t num_inflight;
	doca_error_t result;
	struct nvmf_doca_sq *sq = ctx_user_data.ptr;

	result = doca_ctx_get_num_inflight_tasks(doca_dma_as_ctx(sq->queue.dma), &num_inflight);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA SQ: Failed to get number of inflight DMA tasks - %s",
			     doca_error_get_name(result));
		return;
	}
	if (num_inflight == 0) {
		nvmf_doca_queue_free_elements(&sq->queue);
	}
}

/*
 * Callback invoked once state change occurs to the DMA context held by the SQ queue
 *
 * @user_data [in]: The user data associated with the context
 * @ctx [in]: The DMA context
 * @prev_state [in]: The previous state
 * @next_state [in]: The new state
 */
static void nvmf_doca_sq_queue_dma_state_changed_cb(const union doca_data user_data,
						    struct doca_ctx *ctx,
						    enum doca_ctx_states prev_state,
						    enum doca_ctx_states next_state)
{
	(void)ctx;
	(void)prev_state;

	struct nvmf_doca_sq *sq = user_data.ptr;

	switch (next_state) {
	case DOCA_CTX_STATE_IDLE:
		nvmf_doca_sq_stop_continue(sq);
		break;
	case DOCA_CTX_STATE_STARTING:
	case DOCA_CTX_STATE_RUNNING:
	case DOCA_CTX_STATE_STOPPING:
	default:
		break;
	}
}

/*
 * Creates NVMF doca requests pool and associates it with the given SQ.
 *
 * @sq [in]: The SQ that holds the requests pool
 * @num_requests [in]: Number of requests to be allocated
 * @doca_pg [in]: Doca poll group taking care of the request
 * @return: DOCA_SUCCESS on success and DOCA_ERROR_NO_MEMORY when allocation fails.
 */
static doca_error_t nvmf_doca_request_pool_create(struct nvmf_doca_sq *sq, size_t num_requests)
{
	sq->request_pool_memory = calloc(num_requests, sizeof(struct nvmf_doca_request));
	if (sq->request_pool_memory == NULL) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA SQ SPDK request pool: Failed to allocate memory for requests");
		return DOCA_ERROR_NO_MEMORY;
	}

	TAILQ_INIT(&sq->request_pool);
	for (size_t request_idx = 0; request_idx < num_requests; request_idx++) {
		struct nvmf_doca_request *request = &(sq->request_pool_memory[request_idx]);
		request->request.cmd = (union nvmf_h2c_msg *)&request->command;
		request->request.rsp = (union nvmf_c2h_msg *)&request->cq_entry;
		request->request.qpair = &sq->spdk_qp;
		request->request.stripped_data = NULL;
		TAILQ_INSERT_TAIL(&(sq->request_pool), request, link);
	}
	return DOCA_SUCCESS;
}

/*
 * Destroys NVMF doca requests pool.
 *
 * @sq [in]: The SQ that holds the requests pool
 * @return: DOCA_SUCCESS on success and DOCCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_request_pool_destroy(struct nvmf_doca_sq *sq)
{
	if (sq->request_pool_memory != NULL) {
		TAILQ_INIT(&(sq->request_pool));
		free(sq->request_pool_memory);
		sq->request_pool_memory = NULL;
	}

	return DOCA_SUCCESS;
}

/*
 * This function is responsible for obtaining an NVMF request object from the pool associated
 * with the given SQ.
 *
 * @sq [in]: The SQ that holds the requests pool
 * @return: NVMF doca request
 */
struct nvmf_doca_request *nvmf_doca_request_get(struct nvmf_doca_sq *sq)
{
	struct nvmf_doca_request *request = TAILQ_FIRST(&sq->request_pool);
	if (request == NULL) {
		return NULL;
	}

	TAILQ_REMOVE(&(sq->request_pool), request, link);

	return request;
}

/*
 * This method is responsible for freeing an NVMF request that has been processed.
 * It returns the request back to the pool associated with the given SQ, making it available for reuse.
 *
 * @sq [in]: The SQ that holds the requests pool
 * @request [in]: NVMF doca request
 */
static void nvmf_doca_request_free_impl(struct nvmf_doca_sq *sq, struct nvmf_doca_request *request)
{
	memset(&request->command, 0, sizeof(request->command));
	memset(&request->cq_entry, 0, sizeof(request->cq_entry));
	request->data_from_alloc = false;
	request->cb_arg = NULL;
	request->doca_cb = NULL;
	request->request.data = NULL;
	request->request.iovcnt = 0;
	request->request.length = 0;
	request->prp_dpu_buf = NULL;
	request->prp_host_buf = NULL;
	request->num_of_buffers = 0;
	request->residual_length = 0;
	request->sqe_idx = 0;

	TAILQ_INSERT_TAIL(&sq->request_pool, request, link);
}

/*
 * This method completes an NVMF request by invoking its associated callback and then freeing the request.
 *
 * @request [in]: NVMF doca request to complete
 */
void nvmf_doca_request_complete(struct nvmf_doca_request *request)
{
	if (request->doca_cb != NULL) {
		request->doca_cb(request, request->cb_arg);
	}
}

/*
 * Determines the correct SQ for the request and then invokes the internal mechanism to free the request.
 *
 * @request [in]: NVMF doca request to free
 */
void nvmf_doca_request_free(struct nvmf_doca_request *request)
{
	struct nvmf_doca_sq *sq = SPDK_CONTAINEROF(request->request.qpair, struct nvmf_doca_sq, spdk_qp);

	int used_host_bufs = request->request.iovcnt;
	int used_dpu_bufs = request->request.iovcnt;
	while (used_dpu_bufs > 0) {
		if (request->dpu_buffer[used_dpu_bufs - 1])
			doca_buf_dec_refcount(request->dpu_buffer[used_dpu_bufs - 1], NULL);
		used_dpu_bufs--;
	}

	while (used_host_bufs > 0) {
		if (request->host_buffer[used_host_bufs - 1])
			doca_buf_dec_refcount(request->host_buffer[used_host_bufs - 1], NULL);
		used_host_bufs--;
	}

	if (request->prp_dpu_buf != NULL) {
		doca_buf_dec_refcount(request->prp_dpu_buf, NULL);
	}

	if (request->prp_host_buf != NULL) {
		doca_buf_dec_refcount(request->prp_host_buf, NULL);
	}

	if (request->data_from_alloc) {
		free(request->request.data);
	}

	nvmf_doca_request_free_impl(sq, request);
}

/*
 * Destroy NVMf DOCA SQ
 *
 * @sq [in]: The SQ to destroy
 */
static void nvmf_doca_sq_destroy(struct nvmf_doca_sq *sq)
{
	doca_error_t result;

	if (sq->db != NULL) {
		result = doca_devemu_pci_db_destroy(sq->db);
		if (result != DOCA_SUCCESS)
			DOCA_LOG_ERR("Failed to destroy NVMf DOCA SQ: Failed to destroy DB - %s",
				     doca_error_get_name(result));
		sq->db = NULL;
	}

	nvmf_doca_dma_pool_destroy(&sq->dma_pool);

	nvmf_doca_queue_destroy(&sq->queue);

	nvmf_doca_request_pool_destroy(sq);
}

struct nvmf_doca_sq_create_attr {
	struct doca_pe *pe;			       /**< Progress engine to be used by DMA context */
	struct doca_dev *dev;			       /**< A doca device representing the emulation manager */
	struct doca_devemu_pci_dev *nvme_dev;	       /**< The emulated NVMe device used for creating the CQ DB */
	uint16_t sq_depth;			       /**< The log of the CQ number of elements */
	struct doca_mmap *host_sq_mmap;		       /**< An mmap granting access to the Host CQ memory */
	uintptr_t host_sq_address;		       /**< I/O address of the CQ on the Host */
	struct doca_devemu_pci_db_completion *db_comp; /**< DB completion context used by DPA to poll for DBs */
	uint32_t sq_id;				       /**< The NVMe CQ ID that is associated with this IO */
	struct nvmf_doca_io *io;		       /**< The IO that the SQ is added to */
	struct spdk_nvmf_transport *transport;	       /**< The doca transport includes this SQ */
	void *ctx;				       /**< Opaque structure that can be set by user */
};

/*
 * Create NVMf DOCA SQ for reading SQEs from Host SQ
 *
 * @attr [in]: The SQ attributes
 * @sq [in]: The SQ to initialize
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t nvmf_doca_sq_create(const struct nvmf_doca_sq_create_attr *attr, struct nvmf_doca_sq *sq)
{
	doca_error_t result;

	result = nvmf_doca_request_pool_create(sq, attr->sq_depth);
	if (result != DOCA_SUCCESS) {
		nvmf_doca_sq_destroy(sq);
		return result;
	}

	struct nvmf_doca_queue_create_attr queue_attr = {
		.pe = attr->pe,
		.dev = attr->dev,
		.remote_queue_mmap = attr->host_sq_mmap,
		.remote_queue_address = attr->host_sq_address,
		.queue_depth = attr->sq_depth,
		.element_size = sizeof(struct nvmf_doca_sqe),
		.is_read_from_remote = true,
		.success_cb = nvmf_doca_sq_sqe_read_cb,
		.error_cb = nvmf_doca_sq_sqe_read_error_cb,
		.dma_state_changed_cb = nvmf_doca_sq_queue_dma_state_changed_cb,
		.dma_user_data = sq,
	};
	result = nvmf_doca_queue_create(&queue_attr, &sq->queue);
	if (result != DOCA_SUCCESS) {
		nvmf_doca_sq_destroy(sq);
		return result;
	}

	const uint32_t num_sq_elements = attr->sq_depth;
	struct nvmf_doca_dma_pool_create_attr dma_pool_attr = {
		.pe = attr->pe,
		.dev = attr->dev,
		.max_dma_operations = num_sq_elements * NVMF_REQ_MAX_BUFFERS,
		.max_dma_operation_size = DMA_POOL_DATA_BUFFER_SIZE,
		.host_data_mmap = attr->host_sq_mmap,
		.success_cb = nvmf_doca_dma_pool_copy_cb,
		.error_cb = nvmf_doca_dma_pool_copy_error_cb,
		.dma_state_changed_cb = nvmf_doca_dma_pool_dma_state_changed_cb,
		.dma_user_data = sq,
	};
	result = nvmf_doca_dma_pool_create(&dma_pool_attr, &sq->dma_pool);
	if (result != DOCA_SUCCESS) {
		nvmf_doca_sq_destroy(sq);
		return result;
	}

	const uint32_t sq_db_id = 2 * attr->sq_id;
	result = doca_devemu_pci_db_create_on_dpa(attr->nvme_dev,
						  attr->db_comp,
						  db_configs[0].region.bar_id,
						  db_configs[0].region.start_address,
						  sq_db_id,
						  /*user_data=*/(uint64_t)sq,
						  &sq->db);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create NVMf DOCA SQ: Failed to create DB - %s", doca_error_get_name(result));
		nvmf_doca_sq_destroy(sq);
		return result;
	}
	result = doca_devemu_pci_db_get_dpa_handle(sq->db, &sq->db_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add SQ to NVMf DOCA IO: Failed to SQ DB DPA handle - %s",
			     doca_error_get_name(result));
		nvmf_doca_sq_destroy(sq);
		return result;
	}
	sq->io = attr->io;
	sq->sq_id = attr->sq_id;
	sq->spdk_qp.qid = attr->sq_id;
	sq->spdk_qp.transport = attr->transport;
	sq->ctx = attr->ctx;
	sq->result = DOCA_SUCCESS;

	return DOCA_SUCCESS;
}

void nvmf_doca_io_rm_sq(struct nvmf_doca_sq *sq)
{
	TAILQ_REMOVE(&sq->io->sq_list, sq, link);
	nvmf_doca_sq_destroy(sq);
}

/*
 * Method invoked once unbind DB done message is received from DPA
 *
 * @msg [in]: The unbind done message that was received from DPA
 */
static void nvmf_doca_io_handle_unbind_sq_db_done_msg(const struct comch_msg *msg)
{
	struct nvmf_doca_sq *sq = (struct nvmf_doca_sq *)msg->unbind_sq_db_done_data.cookie;

	sq->db_state = NVMF_DOCA_SQ_DB_UNBOUND;
	nvmf_doca_sq_stop_continue(sq);
}

/*
 * Progress the add operation of the NVMf DOCA SQ by a step depending on current state
 *
 * @sq [in]: The SQ that is being added
 */
static void nvmf_doca_sq_add_continue(struct nvmf_doca_sq *sq)
{
	struct spdk_nvmf_tgt *target;

	// NVMF_DOCA_SQ_DB_BOUND
	sq->result = doca_devemu_pci_db_start(sq->db);
	if (sq->result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add SQ to NVMf DOCA IO: Failed to start DB - %s",
			     doca_error_get_name(sq->result));
		nvmf_doca_sq_stop_continue(sq);
		return;
	}
	target = spdk_nvmf_get_first_tgt();
	spdk_nvmf_tgt_new_qpair(target, &sq->spdk_qp);
}

/*
 * Continue the async flow of stopping the SQ
 *
 * @sq [in]: The NVMf DOCA SQ to stop
 */
static void nvmf_doca_sq_stop_continue(struct nvmf_doca_sq *sq)
{
	if (sq->spdk_qp.state != SPDK_NVMF_QPAIR_ERROR && sq->spdk_qp.state != SPDK_NVMF_QPAIR_UNINITIALIZED) {
		return;
	}

	doca_error_t result;
	struct nvmf_doca_io *io = sq->io;
	struct comch_msg unbind_msg = {
		.type = COMCH_MSG_TYPE_UNBIND_SQ_DB,
		.unbind_sq_db_data =
			{
				.db = sq->db_handle,
				.cookie = (uint64_t)sq,
			},
	};

	result = doca_devemu_pci_db_stop(sq->db);
	if (result != DOCA_SUCCESS && result != DOCA_ERROR_BAD_STATE) {
		DOCA_LOG_ERR("Failed to stop NVMf DOCA SQ: Failed to stop SQ DB - %s", doca_error_get_name(result));
	}

	if (sq->db_state == NVMF_DOCA_SQ_DB_BOUND) {
		result = nvmf_doca_dpa_msgq_send(&sq->io->comch.send, &unbind_msg, sizeof(unbind_msg));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to remove SQ from NVMf DOCA IO: Failed to send unbind SQ DB message to DPA - %s",
				doca_error_get_name(result));
		}
		sq->db_state = NVMF_DOCA_SQ_DB_UNBIND_REQUESTED;
		return;
	}
	if (sq->db_state != NVMF_DOCA_SQ_DB_UNBOUND) {
		return;
	}

	nvmf_doca_dma_pool_stop(&sq->dma_pool);

	if (!is_ctx_idle(doca_dma_as_ctx(sq->dma_pool.dma))) {
		return;
	}

	nvmf_doca_queue_stop(&sq->queue);

	if (!is_ctx_idle(doca_dma_as_ctx(sq->queue.dma))) {
		return;
	}

	io->stop_sq_cb(sq);
}

void nvmf_doca_sq_stop(struct nvmf_doca_sq *sq)
{
	spdk_nvmf_qpair_disconnect(&sq->spdk_qp, (nvmf_qpair_disconnect_cb)nvmf_doca_sq_stop_continue, sq);
}

void nvmf_doca_io_add_sq(struct nvmf_doca_io *io, const struct nvmf_doca_io_add_sq_attr *attr, struct nvmf_doca_sq *sq)
{
	struct nvmf_doca_sq_create_attr sq_attr = {
		.pe = attr->pe,
		.dev = attr->dev,
		.nvme_dev = attr->nvme_dev,
		.sq_depth = attr->sq_depth,
		.host_sq_mmap = attr->host_sq_mmap,
		.host_sq_address = attr->host_sq_address,
		.db_comp = io->db_comp,
		.sq_id = attr->sq_id,
		.io = io,
		.transport = attr->transport,
		.ctx = attr->ctx,
	};
	doca_error_t result = nvmf_doca_sq_create(&sq_attr, sq);
	if (result != DOCA_SUCCESS)
		return;

	TAILQ_INSERT_TAIL(&io->sq_list, sq, link);

	struct comch_msg msg = {
		.type = COMCH_MSG_TYPE_BIND_SQ_DB,
		.bind_sq_db_data =
			{
				.db = sq->db_handle,
				.cookie = (uint64_t)sq,
			},
	};
	sq->result = nvmf_doca_dpa_msgq_send(&sq->io->comch.send, &msg, sizeof(msg));
	if (sq->result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add SQ to NVMf DOCA IO: Failed to send bind SQ DB message to DPA - %s",
			     doca_error_get_name(sq->result));
		nvmf_doca_sq_stop_continue(sq);
		return;
	}
	sq->db_state = NVMF_DOCA_SQ_DB_BIND_REQUESTED;
}

/*
 * Method invoked once bind DB done message is received from DPA
 *
 * @msg [in]: The bind done message that was received from DPA
 */
static void nvmf_doca_io_handle_bind_sq_db_done_msg(const struct comch_msg *msg)
{
	struct nvmf_doca_sq *sq = (struct nvmf_doca_sq *)msg->bind_sq_db_done_data.cookie;

	sq->db_state = NVMF_DOCA_SQ_DB_BOUND;
	nvmf_doca_sq_add_continue(sq);
}
