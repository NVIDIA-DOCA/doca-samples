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

#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include <doca_bitfield.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_flow_definitions.h>
#include <doca_log.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_MESSAGE_QUEUE);

/*
 * The message queue injects software-originated messages into the steering pipeline, where they are
 * matched and modified like packets but stay isolated from the regular datapath. This sample walks
 * the whole feature end to end on a single VNF port:
 *
 * 1. A two-field message format ("key", "seq") is registered as a programmable struct under both
 *    message families, and bound to a message type - all before doca_flow_init().
 * 2. The port is started with one message queue and a completion callback.
 * 3. Two MESSAGE-isolation pipes are created in the egress domain:
 *    - The classify pipe matches "key" (match.message.programmable.key), SETs "seq" to a constant
 *      (actions.message.programmable.seq), COPY's "key" into pkt_meta with an action descriptor, and
 *      forwards to the verdict pipe.
 *    - The verdict pipe matches both the SET "seq" and the copied pkt_meta, and drops. A message
 *      pipeline must terminate in a drop - no other terminal forward is allowed - and its
 *      per-entry counter is how the application observes what reached it.
 * 4. The message type is attached to the classify pipe, which makes it the type's origin pipe.
 * 5. Hitting messages are sent buffered (WAIT_FOR_BATCH) and missing ones unbuffered (NO_WAIT),
 *    completions are polled with doca_flow_message_queue_process(), and the outcome is read from
 *    two independent places: doca_flow_message_queue_query() counts the sends the NIC injected
 *    (match-agnostic), while the entry counters count the messages that actually matched.
 *
 * Built-in messages (doca_flow_builtin_message_type() and BUILTIN_MESSAGE isolation) are a separate
 * producer-driven flow - they cannot be sent from software - and are out of this sample's scope.
 *
 * Requirements:
 * - ConnectX-9 or newer. On ConnectX-9 the message queue also needs a flex parser profile enabled
 *   on the PF (followed by a firmware reset or a host reboot); newer generations do not:
 *	sudo mlxconfig -d <pci> set FLEX_PARSER_PROFILE_ENABLE=10
 * - Root privileges, for the queue's DevX objects.
 */

#define NB_PORTS 1
#define MESSAGE_PORT_ID 0

/* Pipe-entry queue, used by add_entry / entries_process - unrelated to the message queue index. */
#define CTRL_QUEUE_ID 0

#define MESSAGE_QUEUE_IDX 0
#define NR_MESSAGE_QUEUES 1
#define MESSAGE_QUEUE_DEPTH 64

#define NB_CLASSIFY_ENTRIES 1
#define NB_VERDICT_ENTRIES 1
#define NB_TOTAL_ENTRIES (NB_CLASSIFY_ENTRIES + NB_VERDICT_ENTRIES)

/* The key the classify pipe entry is keyed on, and one no entry matches. */
#define MESSAGE_KEY 0x00001234
#define MESSAGE_MISS_KEY 0x0000beef
/* The value the classify pipe SETs into the message's "seq" field. */
#define MESSAGE_SEQ_SET 0x11111111

#define NB_HIT_MESSAGES 4
#define NB_MISS_MESSAGES 2
#define NB_MESSAGES (NB_HIT_MESSAGES + NB_MISS_MESSAGES)

/* Budget for retiring the sends, generous enough that only a stuck queue can exhaust it. */
#define COMPLETION_POLL_BUDGET_US (10 * 1000 * 1000)
#define COMPLETION_POLL_INTERVAL_US 100
#define COMPLETION_POLL_ATTEMPTS (COMPLETION_POLL_BUDGET_US / COMPLETION_POLL_INTERVAL_US)

/* Flow counters are refreshed on the device's own cadence, a separate wait from the retire above. */
#define COUNTER_POLL_INTERVAL_US 100000
#define COUNTER_POLL_ATTEMPTS 50

/* Copy destination inside struct doca_flow_meta, in bits, as the action descriptor expects it. */
#define META_PKT_META_BIT_OFFSET (offsetof(struct doca_flow_meta, pkt_meta) * 8)

/*
 * The application-defined message format. The hardware reads each field as the bytes are laid out
 * in the buffer, so the fields are declared in network order and written with DOCA_HTOBE32() - the
 * same representation the pipe match values and the copy destination use.
 */
struct message_format {
	doca_be32_t key;
	doca_be32_t seq;
};

/*
 * The message fields extend the match and actions structs. Their registration offsets must sit past
 * the built-in members, so the format is appended rather than overlaid.
 */
struct message_match {
	struct doca_flow_match base;
	struct message_format msg;
};

struct message_actions {
	struct doca_flow_actions base;
	struct message_format msg;
};

struct message_completions {
	uint32_t nb_completed;
	uint32_t nb_failed;
};

/* The completion callback is registered on the port config, so its bookkeeping lives here. */
static struct message_completions completions;

/* Stopping the port drains outstanding sends, so a per-send context must outlive the sending scope. */
static uint32_t send_ids[NB_MESSAGES];

/*
 * Message completion callback - called by doca_flow_message_queue_process() once per retired send.
 *
 * A completion means the device accepted and injected the message; whether it matched anything in
 * the pipeline is reported by the entry counters instead.
 *
 * @msg_type [in]: message type of the completed send
 * @message_queue_idx [in]: message queue that retired the send
 * @user_ctx [in]: per-send context passed to doca_flow_message_queue_send()
 * @status [in]: completion status of the send
 */
static void message_completion_cb(const struct doca_flow_message_type *msg_type,
				  uint16_t message_queue_idx,
				  void *user_ctx,
				  doca_error_t status)
{
	const uint32_t *send_id = (const uint32_t *)user_ctx;

	(void)msg_type;

	completions.nb_completed++;
	if (status != DOCA_SUCCESS) {
		completions.nb_failed++;
		DOCA_LOG_ERR("Message send %u on queue %u failed: %s",
			     send_id != NULL ? *send_id : 0,
			     message_queue_idx,
			     doca_error_get_descr(status));
		return;
	}

	DOCA_LOG_INFO("Message send %u on queue %u completed", send_id != NULL ? *send_id : 0, message_queue_idx);
}

/*
 * Port configuration callback - adds the message queues before the port is started.
 *
 * @port_cfg [in]: port configuration to augment
 * @port_id [in]: port ID
 * @config_cb_ctx [in]: unused
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t message_queue_port_config_cb(struct doca_flow_port_cfg *port_cfg, int port_id, void *config_cb_ctx)
{
	doca_error_t result;

	(void)config_cb_ctx;

	if (port_id != MESSAGE_PORT_ID)
		return DOCA_SUCCESS;

	result = doca_flow_port_cfg_set_nr_message_queues(port_cfg, NR_MESSAGE_QUEUES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set the number of message queues: %s", doca_error_get_descr(result));
		return result;
	}

	/* Mandatory once the port carries message queues - the depth has no default. */
	result = doca_flow_port_cfg_set_message_queue_depth(port_cfg, MESSAGE_QUEUE_DEPTH);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set the message queue depth: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_cb_message_process(port_cfg, message_completion_cb);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set the message completion callback: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Register the message format and bind a message type to it.
 *
 * A message type binds only to a programmable struct registered under both message families, and
 * only before doca_flow_init(). Every field becomes two opcodes - "match.message.programmable.<name>"
 * and "actions.message.programmable.<name>" - resolved against the offsets given here.
 *
 * @defs [out]: definitions object to pass to doca_flow_init()
 * @fmt [out]: programmable struct describing the message format
 * @msg_type [out]: message type bound to the format
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t register_message_type(struct doca_flow_definitions **defs,
					  struct doca_flow_definitions_programmable_struct **fmt,
					  struct doca_flow_message_type **msg_type)
{
	struct doca_flow_definitions_cfg *defs_cfg;
	doca_error_t result;

	result = doca_flow_definitions_cfg_create(&defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the definitions cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_create(defs_cfg, defs);
	(void)doca_flow_definitions_cfg_destroy(defs_cfg);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the definitions: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_programmable_struct_create(sizeof(struct message_format), fmt);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the message format: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_programmable_struct_add_field(*fmt,
								     "key",
								     offsetof(struct message_format, key),
								     sizeof(((struct message_format *)0)->key));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add the 'key' message field: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_programmable_struct_add_field(*fmt,
								     "seq",
								     offsetof(struct message_format, seq),
								     sizeof(((struct message_format *)0)->seq));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add the 'seq' message field: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_add_programmable_struct(*defs,
							       "match.message.programmable",
							       offsetof(struct message_match, msg),
							       *fmt);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register the message match fields: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_add_programmable_struct(*defs,
							       "actions.message.programmable",
							       offsetof(struct message_actions, msg),
							       *fmt);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register the message actions fields: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_definitions_add_message_type(*defs, *fmt, msg_type);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add the message type: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Set the pipe properties shared by every pipe of a message pipeline.
 *
 * MESSAGE isolation confines the pipe to matching and modifying the message-format fields and the
 * metadata, and lets it forward only to another MESSAGE pipe or to a drop. The egress domain is
 * where the library places the port's message steering, which is also what attach requires.
 *
 * @pipe_cfg [in]: pipe configuration to augment
 * @name [in]: pipe name
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t set_message_pipe_cfg(struct doca_flow_pipe_cfg *pipe_cfg, const char *name)
{
	doca_error_t result;

	result = set_flow_pipe_cfg(pipe_cfg, name, DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_domain(pipe_cfg, DOCA_FLOW_PIPE_DOMAIN_EGRESS);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg domain: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_isolation_type(pipe_cfg, DOCA_FLOW_PIPE_ISOLATION_TYPE_MESSAGE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg isolation type: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create the verdict pipe, the last pipe of the message pipeline.
 *
 * It is keyed on what the classify pipe wrote: the SET "seq" field and the pkt_meta the action
 * descriptor copied the "key" into. Its entry counter is therefore the observable proving both
 * modifications survived the hop.
 *
 * @port [in]: port of the pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_verdict_pipe(struct doca_flow_port *port, struct doca_flow_pipe **pipe)
{
	struct message_match match;
	struct message_match mask;
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));

	match.msg.seq = UINT32_MAX;
	match.base.meta.pkt_meta = UINT32_MAX;
	mask.msg.seq = UINT32_MAX;
	mask.base.meta.pkt_meta = UINT32_MAX;
	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	/* A message pipeline must terminate in a drop; no other terminal forward is allowed. */
	fwd.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_message_pipe_cfg(pipe_cfg, "MESSAGE_VERDICT_PIPE");
	if (result != DOCA_SUCCESS)
		goto destroy_pipe_cfg;

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_VERDICT_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, &mask.base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Create the classify pipe, the origin pipe the message type is attached to.
 *
 * It matches the message "key" and applies both flavors of action on the message fields: a constant
 * SET of "seq" through the actions struct, and a COPY of "key" into pkt_meta through an action
 * descriptor. On a MESSAGE pipe an action descriptor may name a message field or a metadata field,
 * spelled without the "actions." prefix.
 *
 * @port [in]: port of the pipe
 * @verdict_pipe [in]: pipe the matched messages are forwarded to
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_classify_pipe(struct doca_flow_port *port,
					 struct doca_flow_pipe *verdict_pipe,
					 struct doca_flow_pipe **pipe)
{
	struct message_match match;
	struct message_match mask;
	struct message_actions actions;
	struct doca_flow_actions *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_action_desc desc_array[1];
	struct doca_flow_action_descs descs;
	struct doca_flow_action_descs *descs_arr[NB_ACTIONS_ARR];
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_monitor monitor;
	struct doca_flow_fwd fwd;
	struct doca_flow_fwd fwd_miss;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&mask, 0, sizeof(mask));
	memset(&actions, 0, sizeof(actions));
	memset(desc_array, 0, sizeof(desc_array));
	memset(&descs, 0, sizeof(descs));
	memset(&monitor, 0, sizeof(monitor));
	memset(&fwd, 0, sizeof(fwd));
	memset(&fwd_miss, 0, sizeof(fwd_miss));

	match.msg.key = UINT32_MAX;
	mask.msg.key = UINT32_MAX;

	/* No actions mask, so "seq" is set to this constant for every entry of the pipe. */
	actions.msg.seq = DOCA_HTOBE32(MESSAGE_SEQ_SET);
	actions_arr[0] = &actions.base;

	desc_array[0].type = DOCA_FLOW_ACTION_COPY;
	desc_array[0].field_op.src.field_string = "message.programmable.key";
	desc_array[0].field_op.src.bit_offset = 0;
	desc_array[0].field_op.dst.field_string = "meta.data";
	desc_array[0].field_op.dst.bit_offset = META_PKT_META_BIT_OFFSET;
	desc_array[0].field_op.width = 32;
	descs.nb_action_desc = 1;
	descs.desc_array = desc_array;
	descs_arr[0] = &descs;

	monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
	/* Both forwards stay inside the message pipeline, as isolation requires. */
	fwd.type = DOCA_FLOW_FWD_PIPE;
	fwd.next_pipe = verdict_pipe;
	fwd_miss.type = DOCA_FLOW_FWD_DROP;

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_message_pipe_cfg(pipe_cfg, "MESSAGE_CLASSIFY_PIPE");
	if (result != DOCA_SUCCESS)
		goto destroy_pipe_cfg;

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NB_CLASSIFY_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match.base, &mask.base);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, descs_arr, NB_ACTIONS_ARR);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_monitor(pipe_cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(pipe_cfg, &fwd, &fwd_miss, pipe);

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add the entry that keys the classify pipe on MESSAGE_KEY.
 *
 * The entry carries no actions of its own: the pipe configured a constant SET and an action
 * descriptor, so every entry of the pipe applies them.
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @entry [out]: created entry pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_classify_pipe_entry(struct doca_flow_pipe *pipe,
					    struct entries_status *status,
					    struct doca_flow_pipe_entry **entry)
{
	struct message_match match;

	memset(&match, 0, sizeof(match));
	match.msg.key = DOCA_HTOBE32(MESSAGE_KEY);

	return doca_flow_pipe_basic_add_entry(CTRL_QUEUE_ID, pipe, &match.base, 0, NULL, NULL, NULL, 0, status, entry);
}

/*
 * Add the entry that keys the verdict pipe on the values the classify pipe wrote.
 *
 * @pipe [in]: pipe of the entry
 * @status [in]: user context for adding entry
 * @entry [out]: created entry pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t add_verdict_pipe_entry(struct doca_flow_pipe *pipe,
					   struct entries_status *status,
					   struct doca_flow_pipe_entry **entry)
{
	struct message_match match;

	memset(&match, 0, sizeof(match));
	match.msg.seq = DOCA_HTOBE32(MESSAGE_SEQ_SET);
	match.base.meta.pkt_meta = DOCA_HTOBE32(MESSAGE_KEY);

	return doca_flow_pipe_basic_add_entry(CTRL_QUEUE_ID, pipe, &match.base, 0, NULL, NULL, NULL, 0, status, entry);
}

/*
 * Send a batch of messages carrying the same key.
 *
 * @port [in]: port owning the message queue
 * @msg_type [in]: message type to send
 * @key [in]: value of the message "key" field
 * @count [in]: number of messages to send
 * @flags [in]: DOCA_FLOW_QUEUE_FLAGS_NO_WAIT or DOCA_FLOW_QUEUE_FLAGS_WAIT_FOR_BATCH
 * @send_ids [in]: per-send contexts echoed back to the completion callback
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t send_messages(struct doca_flow_port *port,
				  const struct doca_flow_message_type *msg_type,
				  uint32_t key,
				  uint32_t count,
				  uint32_t flags,
				  uint32_t send_ids[])
{
	struct message_format message;
	doca_error_t result;
	uint32_t i;

	memset(&message, 0, sizeof(message));
	message.key = DOCA_HTOBE32(key);

	for (i = 0; i < count; i++) {
		/* The pipeline overwrites "seq"; the sent value shows the SET actually took effect. */
		message.seq = DOCA_HTOBE32(i);

		result = doca_flow_message_queue_send(port, MESSAGE_QUEUE_IDX, msg_type, &message, flags, &send_ids[i]);
		if (result == DOCA_ERROR_AGAIN) {
			/* The queue is full: retiring completions is what frees a slot. */
			result = doca_flow_message_queue_process(port, MESSAGE_QUEUE_IDX, 0);
			if (result == DOCA_SUCCESS)
				result = doca_flow_message_queue_send(port,
								      MESSAGE_QUEUE_IDX,
								      msg_type,
								      &message,
								      flags,
								      &send_ids[i]);
		}
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to send message %u of key 0x%08x: %s",
				     i,
				     key,
				     doca_error_get_descr(result));
			return result;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Poll the message queue until every send has been retired.
 *
 * Processing is the only reader of the completion queue, so buffered sends are submitted and
 * completion callbacks fire only while it is called.
 *
 * @port [in]: port owning the message queue
 * @expected [in]: number of completions to wait for
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t poll_completions(struct doca_flow_port *port, uint32_t expected)
{
	doca_error_t result;
	uint32_t i;

	for (i = 0; i < COMPLETION_POLL_ATTEMPTS && completions.nb_completed < expected; i++) {
		result = doca_flow_message_queue_process(port, MESSAGE_QUEUE_IDX, 0);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process message completions: %s", doca_error_get_descr(result));
			return result;
		}
		if (completions.nb_completed < expected)
			usleep(COMPLETION_POLL_INTERVAL_US);
	}

	if (completions.nb_completed != expected || completions.nb_failed != 0) {
		DOCA_LOG_ERR("Expected %u successful completions, got %u completions with %u failures",
			     expected,
			     completions.nb_completed,
			     completions.nb_failed);
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

/*
 * Report the queue's lifetime completion counters.
 *
 * These count the sends the device injected, regardless of what they matched, and are independent
 * of the per-send callback.
 *
 * @port [in]: port owning the message queue
 * @expected [in]: number of injected sends to expect
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t report_queue_stats(struct doca_flow_port *port, uint64_t expected)
{
	struct doca_flow_queue_completion_stats stats;
	doca_error_t result;

	memset(&stats, 0, sizeof(stats));

	result = doca_flow_message_queue_query(port, MESSAGE_QUEUE_IDX, &stats);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to query the message queue: %s", doca_error_get_descr(result));
		return result;
	}

	DOCA_LOG_INFO("Message queue %u: %" PRIu64 " completed, %" PRIu64 " failed, expected %" PRIu64 " completed",
		      MESSAGE_QUEUE_IDX,
		      stats.num_completed,
		      stats.num_failed,
		      expected);

	if (stats.num_completed != expected || stats.num_failed != 0)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;
}

/*
 * Report an entry counter, waiting for it to reach the expected value.
 *
 * A completion only proves the device consumed the send, so the entry counters are what report how
 * the messages were dispatched through the pipeline.
 *
 * @entry [in]: counted entry to query
 * @expected [in]: number of packets to expect
 * @what [in]: description of the entry, for the log line
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t report_entry_counter(struct doca_flow_pipe_entry *entry, uint64_t expected, const char *what)
{
	struct doca_flow_resource_query query;
	doca_error_t result;
	uint32_t i;

	memset(&query, 0, sizeof(query));

	for (i = 0; i < COUNTER_POLL_ATTEMPTS; i++) {
		result = doca_flow_resource_query_entry(entry, &query);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query the %s counter: %s", what, doca_error_get_descr(result));
			return result;
		}
		if (query.counter.total_pkts >= expected)
			break;
		usleep(COUNTER_POLL_INTERVAL_US);
	}

	DOCA_LOG_INFO("%s: %" PRIu64 " messages / %" PRIu64 " bytes, expected %" PRIu64 " messages",
		      what,
		      query.counter.total_pkts,
		      query.counter.total_bytes,
		      expected);

	if (query.counter.total_pkts != expected)
		return DOCA_ERROR_BAD_STATE;

	return DOCA_SUCCESS;
}

/*
 * Attach the message type, drive the messages through the pipeline and report the outcome.
 *
 * @port [in]: port owning the message queue
 * @msg_type [in]: message type to attach and send
 * @classify_pipe [in]: origin pipe of the message type
 * @classify_entry [in]: keyed entry of the classify pipe
 * @verdict_entry [in]: keyed entry of the verdict pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t run_message_pipeline(struct doca_flow_port *port,
					 struct doca_flow_message_type *msg_type,
					 struct doca_flow_pipe *classify_pipe,
					 struct doca_flow_pipe_entry *classify_entry,
					 struct doca_flow_pipe_entry *verdict_entry)
{
	doca_error_t result;
	uint32_t i;

	for (i = 0; i < NB_MESSAGES; i++)
		send_ids[i] = i;

	/* Binds the type to its origin pipe; sending before this returns DOCA_ERROR_NOT_CONNECTED. */
	result = doca_flow_message_type_attach(port, msg_type, classify_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach the message type to the classify pipe: %s",
			     doca_error_get_descr(result));
		return result;
	}

	/* Buffered sends wait for a later unbuffered send, or for processing, to submit the batch. */
	result = send_messages(port,
			       msg_type,
			       MESSAGE_KEY,
			       NB_HIT_MESSAGES,
			       DOCA_FLOW_QUEUE_FLAGS_WAIT_FOR_BATCH,
			       send_ids);
	if (result != DOCA_SUCCESS)
		return result;

	/* Same pipe, no entry matches this key: dropped by the classify pipe's miss forward. */
	result = send_messages(port,
			       msg_type,
			       MESSAGE_MISS_KEY,
			       NB_MISS_MESSAGES,
			       DOCA_FLOW_QUEUE_FLAGS_NO_WAIT,
			       &send_ids[NB_HIT_MESSAGES]);
	if (result != DOCA_SUCCESS)
		return result;

	result = poll_completions(port, NB_MESSAGES);
	if (result != DOCA_SUCCESS)
		return result;

	result = report_queue_stats(port, NB_MESSAGES);
	if (result != DOCA_SUCCESS)
		return result;

	result = report_entry_counter(classify_entry, NB_HIT_MESSAGES, "Messages that matched the classify key");
	if (result != DOCA_SUCCESS)
		return result;

	return report_entry_counter(verdict_entry,
				    NB_HIT_MESSAGES,
				    "Messages that carried the SET seq and the copied pkt_meta");
}

/*
 * Run flow_message_queue sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t flow_message_queue(int nb_queues)
{
	struct doca_flow_definitions_programmable_struct *fmt = NULL;
	struct doca_flow_definitions *defs = NULL;
	struct doca_flow_message_type *msg_type = NULL;
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT};
	struct doca_flow_port *ports[NB_PORTS];
	struct doca_dev *dev_arr[NB_PORTS];
	uint32_t actions_mem_size[NB_PORTS];
	struct doca_flow_pipe *classify_pipe;
	struct doca_flow_pipe *verdict_pipe;
	struct doca_flow_pipe_entry *classify_entry;
	struct doca_flow_pipe_entry *verdict_entry;
	struct entries_status status;
	doca_error_t result;
	int port_id;

	memset(&status, 0, sizeof(status));
	memset(&completions, 0, sizeof(completions));
	resource.nr_counters = NB_TOTAL_ENTRIES;

	/* The message format must be registered before DOCA Flow is initialized. */
	result = register_message_type(&defs, &fmt, &msg_type);
	if (result != DOCA_SUCCESS)
		goto destroy_defs;

	result = init_doca_flow_with_defs(nb_queues, "vnf", &resource, nr_shared_resources, defs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow with definitions: %s", doca_error_get_descr(result));
		goto destroy_defs;
	}

	memset(dev_arr, 0, sizeof(dev_arr));
	for (port_id = 0; port_id < NB_PORTS; port_id++) {
		result = doca_dpdk_port_as_dev(port_id, &dev_arr[port_id]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get device for port %d: %s", port_id, doca_error_get_descr(result));
			goto destroy_flow;
		}
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(NB_TOTAL_ENTRIES));
	result = init_doca_flow_ports_with_custom_config(NB_PORTS,
							 ports,
							 false /* is_port_fwd */,
							 dev_arr,
							 NULL /* dev_rep_arr */,
							 message_queue_port_config_cb,
							 NULL /* port_rep_cb */,
							 NULL /* config_cb_ctx */,
							 actions_mem_size,
							 &resource,
							 NULL /* no CT */);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		goto destroy_flow;
	}

	/* The verdict pipe is created first, so the classify pipe can forward to it. */
	result = create_verdict_pipe(ports[MESSAGE_PORT_ID], &verdict_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the verdict pipe: %s", doca_error_get_descr(result));
		goto stop_ports;
	}

	result = create_classify_pipe(ports[MESSAGE_PORT_ID], verdict_pipe, &classify_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create the classify pipe: %s", doca_error_get_descr(result));
		goto stop_ports;
	}

	result = add_verdict_pipe_entry(verdict_pipe, &status, &verdict_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add the verdict pipe entry: %s", doca_error_get_descr(result));
		goto stop_ports;
	}

	result = add_classify_pipe_entry(classify_pipe, &status, &classify_entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add the classify pipe entry: %s", doca_error_get_descr(result));
		goto stop_ports;
	}

	result = flow_process_entries(ports[MESSAGE_PORT_ID], &status, NB_TOTAL_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process the pipe entries: %s", doca_error_get_descr(result));
		goto stop_ports;
	}

	result = run_message_pipeline(ports[MESSAGE_PORT_ID], msg_type, classify_pipe, classify_entry, verdict_entry);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to run the message pipeline: %s", doca_error_get_descr(result));

stop_ports:
	stop_doca_flow_ports(NB_PORTS, ports);
destroy_flow:
	destroy_doca_flow();
destroy_defs:
	/* The message type outlives the definitions - DOCA Flow owns it once initialized. */
	if (fmt != NULL)
		doca_flow_definitions_programmable_struct_destroy(fmt);
	if (defs != NULL)
		doca_flow_definitions_destroy(defs);
	return result;
}
