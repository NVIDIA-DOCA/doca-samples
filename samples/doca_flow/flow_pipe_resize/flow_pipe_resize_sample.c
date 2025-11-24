/*
 * Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_flow.h>

#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_PIPE_RESIZE);

#define PIPE_SIZE 10
#define PERCENTAGE 80
#define MAX_ENTRIES 80
static bool resize_cb_received;
static bool congestion_notified;

static int congestion_reached_flag;

static struct doca_flow_port *ports[3];
static struct doca_flow_pipe_entry *entry[MAX_ENTRIES];
struct entry_ctx {
	struct entries_status entry_status; /* Entry status */
	uint32_t index;			    /* Entry index */
};
static struct entry_ctx entry_ctx[MAX_ENTRIES];

/*
 * pipe number of entries changed callback
 *
 * @pipe_user_ctx [in]: DOCA Flow pipe user context pointer
 * @nr_entries [in]: DOCA Flow new number of entries
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t pipe_resize_sample_nr_entries_changed_cb(void *pipe_user_ctx, uint32_t nr_entries)
{
	DOCA_LOG_INFO("Pipe user context %p: entries increased to %u", pipe_user_ctx, nr_entries);

	return DOCA_SUCCESS;
}

/*
 * pipe entry relocated callback
 *
 * @pipe_user_ctx [in]: DOCA Flow pipe user context pointer
 * @pipe_queue [in]: DOCA Flow pipe queue id
 * @entry_user_ctx [in]: DOCA Flow entry user context pointer
 * @new_entry_user_ctx [out]: DOCA Flow updated entry user context pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t pipe_resize_sample_entry_relocate_cb(void *pipe_user_ctx,
							 uint16_t pipe_queue,
							 void *entry_user_ctx,
							 void **new_entry_user_ctx)
{
	struct entry_ctx *entry_ctx = (struct entry_ctx *)entry_user_ctx;

	DOCA_LOG_INFO("Pipe %p entry context %p (%u) relocated on queue %u",
		      pipe_user_ctx,
		      entry_ctx,
		      entry_ctx->index,
		      pipe_queue);

	*new_entry_user_ctx = entry_ctx;

	return DOCA_SUCCESS;
}

static doca_flow_pipe_resize_nr_entries_changed_cb nr_entries_changed_cb = pipe_resize_sample_nr_entries_changed_cb;
static doca_flow_pipe_resize_entry_relocate_cb entry_relocation_cb = pipe_resize_sample_entry_relocate_cb;

/*
 * Create DOCA Flow control pipe
 *
 * @port [in]: port of the pipe
 * @is_basic_pipe [in]: flag to indicate if to resize a basic pipe
 * @pipe [out]: created pipe pointer
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_resizable_pipe(struct doca_flow_port *port,
					  bool is_basic_pipe,
					  uint16_t nb_queues,
					  struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;
	struct doca_flow_match match;
	struct doca_flow_monitor monitor;
	struct doca_flow_actions actions, *actions_arr[NB_ACTIONS_ARR];
	struct doca_flow_fwd fwd;
	uint16_t port_id = 0;
	uint16_t queue_id;

	if (is_basic_pipe) {
		memset(&match, 0, sizeof(match));
		memset(&monitor, 0, sizeof(monitor));
		memset(&actions, 0, sizeof(actions));
		memset(&fwd, 0, sizeof(fwd));

		/* 5 tuple match */
		match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
		match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
		match.outer.ip4.src_ip = 0xffffffff;
		match.outer.ip4.dst_ip = 0xffffffff;
		match.outer.tcp.l4_port.dst_port = 0xffff;
		match.outer.tcp.l4_port.src_port = 0xffff;

		actions_arr[0] = &actions;
	}

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	if (is_basic_pipe)
		result = set_flow_pipe_cfg(pipe_cfg, "RESIZABLE_BASIC_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	else
		result = set_flow_pipe_cfg(pipe_cfg, "RESIZABLE_CTRL_PIPE", DOCA_FLOW_PIPE_CONTROL, true);

	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, PIPE_SIZE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg nr_entries: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_congestion_level_threshold(pipe_cfg, PERCENTAGE);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg congestion_level_threshold: %s",
			     doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_is_resizable(pipe_cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg is_resizable: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_user_ctx(pipe_cfg, &congestion_reached_flag);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg user_ctx: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	for (queue_id = 1; queue_id < nb_queues; queue_id += 2) {
		/* Exclude odd queue numbers */
		result = doca_flow_pipe_cfg_set_excluded_queue(pipe_cfg, queue_id);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg queue (%u) exclusion: %s",
				     queue_id,
				     doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
	}

	if (is_basic_pipe) {
		result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}
		result = doca_flow_pipe_cfg_set_actions(pipe_cfg, actions_arr, NULL, NULL, NB_ACTIONS_ARR);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg actions: %s", doca_error_get_descr(result));
			goto destroy_pipe_cfg;
		}

		fwd.type = DOCA_FLOW_FWD_PORT;
		fwd.port_id = port_id ^ 1;

		result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL /* fwd_miss */, pipe);
	} else
		result = doca_flow_pipe_create(pipe_cfg, NULL /* fwd */, NULL /* fwd_miss */, pipe);

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Remove DOCA Flow pipe entry from the resizable control pipe.
 *
 * @pipe [in]: entry's pipe
 * @port_id [in]: port id of incoming packets
 * @status [in]: user context for adding entry
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static void remove_resizable_pipe_entries(void)
{
	int i;
	int rc;

	for (i = 0; i < MAX_ENTRIES; i++) {
		rc = doca_flow_pipe_remove_entry(0, DOCA_FLOW_NO_WAIT, entry[i]);
		if (rc)
			DOCA_LOG_WARN("Failed removing entry %d %p", i, entry[i]);
	}
}

/*
 * Add DOCA Flow pipe entry to the resizable control pipe that matches
 * multiple ipv4 traffic and forward to peer port.
 *
 * @pipe [in]: entry's pipe
 * @port_id [in]: port id of incoming packets
 * @nb_queues [in]: number of queues the sample will use
 * @is_basic_pipe [in]: flag to indicate if to add entries to basic pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_resizable_pipe_entries(struct doca_flow_pipe *pipe,
					       int port_id,
					       uint16_t nb_queues,
					       bool is_basic_pipe)
{
	int i;
	struct doca_flow_fwd fwd;
	int rc;
	uint16_t queue_id;

	struct doca_flow_match match;
	struct doca_flow_actions actions;
	doca_error_t result;

	/* example 5-tuple to forward */
	doca_be32_t dst_ip_addr = BE_IPV4_ADDR(8, 8, 8, 8);
	doca_be32_t src_ip_addr = BE_IPV4_ADDR(1, 2, 3, 4);
	doca_be16_t dst_port = DOCA_HTOBE16(80);
	doca_be16_t src_port = DOCA_HTOBE16(1234);

	memset(&match, 0, sizeof(match));
	memset(&actions, 0, sizeof(actions));
	memset(&fwd, 0, sizeof(fwd));

	/* 5 tuple match */
	match.outer.ip4.dst_ip = dst_ip_addr;
	match.outer.ip4.src_ip = src_ip_addr;
	match.outer.tcp.l4_port.dst_port = dst_port;
	match.outer.tcp.l4_port.src_port = src_port;
	match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_TCP;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;

	/* forwarding traffic to other port */
	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = port_id ^ 1;

	memset(entry_ctx, 0, sizeof(entry_ctx));
	for (i = 0; i < MAX_ENTRIES; i++)
		entry_ctx[i].index = i;

	for (i = 0; i < MAX_ENTRIES; i++) {
		match.outer.ip4.dst_ip++;

		if (is_basic_pipe)
			result = doca_flow_pipe_add_entry(0,
							  pipe,
							  &match,
							  0,
							  &actions,
							  NULL /* monitor */,
							  NULL /* fwd */,
							  0,
							  &entry_ctx[i],
							  &entry[i]);
		else
			result = doca_flow_pipe_control_add_entry(0 /* pipe_queue */,
								  1 /*priority */,
								  pipe,
								  &match,
								  NULL /* match_mask */,
								  NULL,
								  &actions,
								  NULL /* actions_mask */,
								  NULL /* action_descs */,
								  NULL /* monitor */,
								  &fwd,
								  &entry_ctx[i],
								  &entry[i]);
		if (result != DOCA_SUCCESS)
			return result;

		if (congestion_notified == true) {
			DOCA_LOG_INFO("Calling resize on pipe %p", pipe);
			rc = doca_flow_pipe_resize(pipe,
						   50 /* New congestion in percentage */,
						   nr_entries_changed_cb,
						   entry_relocation_cb);

			if (rc == 0)
				DOCA_LOG_INFO("Pipe %p successfully called resize operation", pipe);
			else
				DOCA_LOG_WARN("Pipe %p call to resize failed. rc=%d", pipe, rc);

			do {
				for (queue_id = 0; queue_id < nb_queues; queue_id++) {
					/* Skip processing excluded queue #1.
					 * The RESIZED callback can't be
					 * invoked on excluded queues, but
					 * there is no prevention from calling
					 * entries_process on excluded queues
					 * in favour of other pipes (where
					 * those queues are not excluded). */
					if (queue_id == 1)
						continue;
					result = doca_flow_entries_process(ports[0], queue_id, DEFAULT_TIMEOUT_US, 10);
					if (result != DOCA_SUCCESS) {
						DOCA_LOG_ERR("Failed to process entries on queue id %u: %s",
							     queue_id,
							     doca_error_get_descr(result));
						goto failure;
					}
				}
			} while (resize_cb_received == false);
			resize_cb_received = false;
			congestion_notified = false;
		}
	}

	if (is_basic_pipe) {
		/*
		 * Complete the last add entries calls by calling entries process.
		 * It is only required for basic pipe.
		 */

		for (i = 0; i < MAX_ENTRIES; i++) {
			if (entry_ctx[i].entry_status.nb_processed != 0)
				continue;

			DOCA_LOG_INFO("%d entries left to be processed", MAX_ENTRIES - i);
			result = doca_flow_entries_process(ports[0], 0, DEFAULT_TIMEOUT_US, MAX_ENTRIES - i);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to process entries");
				goto failure;
			}
			break;
		}

		for (; i < MAX_ENTRIES; i++) {
			if (entry_ctx[i].entry_status.nb_processed != 1 || entry_ctx[i].entry_status.failure) {
				DOCA_LOG_ERR("Entry %d completed with error", i);
				result = DOCA_ERROR_BAD_STATE;
				goto failure;
			}
		}
	}

	return DOCA_SUCCESS;

failure:
	stop_doca_flow_ports(3, ports);
	doca_flow_destroy();
	return result;
}

/*
 * pipe process callback
 *
 * @pipe [in]: DOCA Flow pipe pointer
 * @status [in]: DOCA Flow pipe status
 * @op [in]: DOCA Flow pipe operation
 * @user_ctx [out]: user context
 */
static void pipe_process_cb(struct doca_flow_pipe *pipe,
			    enum doca_flow_pipe_status status,
			    enum doca_flow_pipe_op op,
			    void *user_ctx)
{
	const char *op_str;
	bool is_err = false;

	(void)user_ctx;
	if (status != DOCA_FLOW_PIPE_STATUS_SUCCESS)
		is_err = true;

	switch (op) {
	case DOCA_FLOW_PIPE_OP_CONGESTION_REACHED:
		op_str = "CONGESTION_REACHED";
		congestion_notified = true;
		break;
	case DOCA_FLOW_PIPE_OP_RESIZED:
		op_str = "RESIZED";
		resize_cb_received = true;
		break;
	case DOCA_FLOW_PIPE_OP_DESTROYED:
		op_str = "DESTROYED";
		break;
	default:
		op_str = "UNKNOWN";
		break;
	}

	if (is_err)
		DOCA_LOG_INFO("Pipe %p received a %s operation callback. Errors encountered", pipe, op_str);
	else
		DOCA_LOG_INFO("Pipe %p successfully received a %s operation callback", pipe, op_str);
}

/*
 * Run flow_pipe_resize sample
 *
 * @nb_queues [in]: number of queues the sample will use
 * @ctx [in]: flow switch context the sample will use
 * @is_basic_pipe [in]: flag to indicate if to resize a basic pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_pipe_resize(uint16_t nb_queues, struct flow_switch_ctx *ctx, bool is_basic_pipe)
{
	const int nb_ports = 3;
	uint32_t actions_mem_size[nb_ports];
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_pipe *resizable_pipe;
	doca_error_t result;
	int port_id;

	/* DOCA flow mode: switch, hardware steering, control pipe dynamic size */
	result = init_doca_flow_cb(nb_queues,
				   "switch,hws,cpds",
				   &resource,
				   nr_shared_resources,
				   check_for_valid_entry,
				   pipe_process_cb,
				   NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(MAX_ENTRIES));
	result = init_doca_flow_switch_ports(ctx->devs_ctx.devs_manager,
					     ctx->devs_ctx.nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size,
					     &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_destroy();
		return result;
	}

	port_id = 0;
	result = create_resizable_pipe(ports[port_id], is_basic_pipe, nb_queues, &resizable_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create resizable pipe: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	resize_cb_received = false;
	congestion_notified = false;

	result = add_resizable_pipe_entries(resizable_pipe, port_id, nb_queues, is_basic_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add entry: %s", doca_error_get_descr(result));
		stop_doca_flow_ports(nb_ports, ports);
		doca_flow_destroy();
		return result;
	}

	DOCA_LOG_INFO("Wait few seconds for packets to arrive");
	sleep(5);

	remove_resizable_pipe_entries();

	result = stop_doca_flow_ports(nb_ports, ports);
	doca_flow_destroy();
	return result;
}
