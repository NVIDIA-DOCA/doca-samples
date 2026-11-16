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

#include <string.h>
#include <unistd.h>

#include <doca_log.h>
#include <doca_dpdk.h>
#include <doca_flow.h>
#include <doca_flow_timer.h>

#include <flow_common.h>

DOCA_LOG_REGISTER(FLOW_EXTERNAL_ACTION_TIMER);

#define NB_PORTS 2
#define NR_TIMERS 1024
#define NR_EVENT_QUEUES 1
#define EVENT_QUEUE_DEPTH 128
#define NR_TIMER_ENTRIES 1024
#define TIMER_COUNTDOWN_SHIFT 22 /* ~4.2 s expiry: formula in doca_flow_timer.h */
#define TRAFFIC_WAIT_SEC 10
#define TIMER_EXPIRY_MARGIN_SEC 5  /* ~4.2 s expiry latency + slack */
#define EVENT_POLL_SLEEP_US 100000 /* 100 ms */
/* Poll across the whole window (traffic wait + expiry latency) so an expiry is observed no matter when
 * the START packet arrives.
 */
#define EVENT_POLL_ITERATIONS (((TRAFFIC_WAIT_SEC + TIMER_EXPIRY_MARGIN_SEC) * 1000000) / EVENT_POLL_SLEEP_US)

/*
 * Timer expiry callback - called by doca_flow_event_process when a timer fires.
 *
 * @event [in]: the flow event
 * @event_queue [in]: event queue index that delivered this event
 * @user_ctx [in]: user context (port pointer)
 */
static void timer_expiry_callback(const struct doca_flow_event *event, uint16_t event_queue, void *user_ctx)
{
	struct doca_flow_port *port = *(struct doca_flow_port **)user_ctx;
	uint32_t resource_id, timer_id;
	doca_error_t result;

	result = doca_flow_event_get_timer_id(port, event, &resource_id, &timer_id);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_WARN("Failed to get timer id: %s", doca_error_get_descr(result));
		return;
	}
	DOCA_LOG_INFO("Timer expired: event_queue=%u, resource_id=%u, timer_id=%u",
		      (unsigned)event_queue,
		      resource_id,
		      timer_id);
}

/*
 * Port configuration callback - sets up timer event queues before port start.
 *
 * @port_cfg [in]: port configuration to augment
 * @port_id [in]: port ID
 * @config_cb_ctx [in]: user context forwarded to the event callback
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t timer_port_config_cb(struct doca_flow_port_cfg *port_cfg, int port_id, void *config_cb_ctx)
{
	doca_error_t result;

	/* Only configure timer events on port 0 (PF0) where the timer resource lives */
	if (port_id != 0)
		return DOCA_SUCCESS;

	result = doca_flow_port_cfg_set_nr_event_queues(port_cfg, DOCA_FLOW_EVENT_TYPE_TIMER, NR_EVENT_QUEUES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set nr event queues: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_port_cfg_set_event_queue_depth(port_cfg, DOCA_FLOW_EVENT_TYPE_TIMER, EVENT_QUEUE_DEPTH);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set event queue depth: %s", doca_error_get_descr(result));
		return result;
	}

	/* Register expiry callback; user_ctx points to ports[0] slot, valid by the time events fire */
	result = doca_flow_port_cfg_set_cb_event_process(port_cfg,
							 DOCA_FLOW_EVENT_TYPE_TIMER,
							 timer_expiry_callback,
							 config_cb_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set event callback: %s", doca_error_get_descr(result));
		return result;
	}

	return DOCA_SUCCESS;
}

/*
 * Create the ordered-list timer pipe with two lists (START / STOP).
 *
 * The caller must keep pipe_actions alive for the lifetime of the pipe and
 * destroy them with doca_flow_external_action_timer_pipe_action_destroy()
 * during final cleanup.
 *
 * @port [in]: port to create the pipe on
 * @timer_resource [in]: timer resource handle
 * @pipe [out]: created pipe pointer
 * @pipe_actions [out]: two pipe-action handles that must outlive the pipe
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_timer_pipe(struct doca_flow_port *port,
				      struct doca_flow_external_resource_timer *timer_resource,
				      struct doca_flow_pipe **pipe,
				      void *pipe_actions[2])
{
	static const struct {
		enum doca_flow_external_action_timer_operation op;
	} list_cfgs[] = {
		[0] = {DOCA_FLOW_EXTERNAL_ACTION_TIMER_OP_START},
		[1] = {DOCA_FLOW_EXTERNAL_ACTION_TIMER_OP_STOP},
	};
	struct doca_flow_ordered_list_element elements[2] = {};
	struct doca_flow_ordered_list lists[2] = {};
	struct doca_flow_ordered_list *ordered_lists[2];
	struct doca_flow_pipe_cfg *pipe_cfg;
	struct doca_flow_fwd fwd = {};
	doca_error_t result;
	uint32_t i;

	pipe_actions[0] = NULL;
	pipe_actions[1] = NULL;

	for (i = 0; i < 2; i++) {
		struct doca_flow_external_action_timer_pipe_action_cfg cfg = {
			.resource_offset = UINT32_MAX, /* changeable per entry */
			.countdown_shift = UINT8_MAX,  /* changeable per entry */
			.op = list_cfgs[i].op,
			.use_access_index = false,
			.resource = timer_resource,
		};

		result = doca_flow_external_action_timer_pipe_action_create(&cfg, &elements[i].external_actions);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create timer pipe action %u: %s", i, doca_error_get_descr(result));
			goto cleanup_actions;
		}

		pipe_actions[i] = elements[i].external_actions;
		elements[i].type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS;
		lists[i].idx = i;
		lists[i].size = 1;
		lists[i].elements = &elements[i];
		ordered_lists[i] = &lists[i];
	}

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe cfg: %s", doca_error_get_descr(result));
		goto cleanup_actions;
	}

	result = doca_flow_pipe_cfg_set_name(pipe_cfg, "TIMER_ORDERED_LIST_PIPE");
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe name: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_ORDERED_LIST);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe type: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_nr_entries(pipe_cfg, NR_TIMER_ENTRIES);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set nr entries: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_ordered_lists(pipe_cfg, ordered_lists, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set ordered lists: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	fwd.type = DOCA_FLOW_FWD_PORT;
	fwd.port_id = 1;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create timer pipe: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return DOCA_SUCCESS;

cleanup_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
cleanup_actions:
	for (i = 0; i < 2; i++) {
		if (pipe_actions[i]) {
			doca_flow_external_action_timer_pipe_action_destroy(pipe_actions[i]);
			pipe_actions[i] = NULL;
		}
	}
	return result;
}

/*
 * Create a basic pipe on PF0 that steers traffic to the timer ordered-list pipe.
 * - Original packets (src=1.2.3.4, dst=8.8.8.8) -> ordered list index 0 (START)
 * - ACK packets     (src=8.8.8.8, dst=1.2.3.4) -> ordered list index 1 (STOP)
 *
 * @port [in]: port to create the pipe on
 * @timer_pipe [in]: the timer ordered-list pipe to forward to
 * @basic_pipe [out]: created pipe pointer
 * @status [in,out]: user context for entry add (must outlive pipe destroy)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t create_basic_pipe(struct doca_flow_port *port,
				      struct doca_flow_pipe *timer_pipe,
				      struct doca_flow_pipe **basic_pipe,
				      struct entries_status *status)
{
	struct doca_flow_match match;
	struct doca_flow_fwd fwd;
	struct doca_flow_pipe_cfg *pipe_cfg;
	doca_error_t result;

	memset(&match, 0, sizeof(match));
	memset(&fwd, 0, sizeof(fwd));

	result = doca_flow_pipe_cfg_create(&pipe_cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create pipe cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(pipe_cfg, "TIMER_BASIC_PIPE", DOCA_FLOW_PIPE_BASIC, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe cfg: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
	match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
	match.outer.ip4.src_ip = UINT32_MAX;
	match.outer.ip4.dst_ip = UINT32_MAX;

	result = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set pipe match: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	/* Forward to the timer ordered-list pipe; idx is changeable per entry */
	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = timer_pipe;
	fwd.ordered_list_pipe.idx = UINT32_MAX;

	result = doca_flow_pipe_create(pipe_cfg, &fwd, NULL, basic_pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create basic pipe: %s", doca_error_get_descr(result));
		goto cleanup_pipe_cfg;
	}

	doca_flow_pipe_cfg_destroy(pipe_cfg);

	/* Entry 1: original packet (src=1.2.3.4, dst=8.8.8.8) -> ordered list index 0 (START) */
	memset(&match, 0, sizeof(match));
	memset(status, 0, sizeof(*status));

	match.outer.ip4.src_ip = BE_IPV4_ADDR(1, 2, 3, 4);
	match.outer.ip4.dst_ip = BE_IPV4_ADDR(8, 8, 8, 8);

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = timer_pipe;
	fwd.ordered_list_pipe.idx = 0;

	result = doca_flow_pipe_basic_add_entry(0,
						*basic_pipe,
						&match,
						0,
						NULL,
						NULL,
						&fwd,
						DOCA_FLOW_ENTRY_FLAGS_WAIT_FOR_BATCH,
						status,
						NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add basic pipe entry (original): %s", doca_error_get_descr(result));
		return result;
	}

	/* Entry 2: ACK packet (src=8.8.8.8, dst=1.2.3.4) -> ordered list index 1 (STOP) */
	memset(&match, 0, sizeof(match));

	match.outer.ip4.src_ip = BE_IPV4_ADDR(8, 8, 8, 8);
	match.outer.ip4.dst_ip = BE_IPV4_ADDR(1, 2, 3, 4);

	fwd.type = DOCA_FLOW_FWD_ORDERED_LIST_PIPE;
	fwd.ordered_list_pipe.pipe = timer_pipe;
	fwd.ordered_list_pipe.idx = 1;

	result = doca_flow_pipe_basic_add_entry(0,
						*basic_pipe,
						&match,
						0,
						NULL,
						NULL,
						&fwd,
						DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
						status,
						NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add basic pipe entry (ACK): %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 2);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process basic pipe entries: %s", doca_error_get_descr(result));
		return result;
	}

	if (status->nb_processed != 2 || status->failure) {
		DOCA_LOG_ERR("Basic pipe entry processing failed");
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;

cleanup_pipe_cfg:
	doca_flow_pipe_cfg_destroy(pipe_cfg);
	return result;
}

/*
 * Add timer entries to the ordered-list pipe.
 * Both START (index 0) and STOP (index 1) entries use the same timer slot
 * so that sending the original packet starts the timer and the ACK stops it.
 *
 * @port [in]: port used for entry processing
 * @pipe [in]: the timer ordered-list pipe
 * @status [in,out]: user context for entry add (must outlive pipe destroy)
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
static doca_error_t add_timer_entries(struct doca_flow_port *port,
				      struct doca_flow_pipe *pipe,
				      struct entries_status *status)
{
	doca_error_t result;
	uint32_t i;

	for (i = 0; i < 2; i++) {
		struct doca_flow_fwd fwd = {
			.type = DOCA_FLOW_FWD_PORT,
			.port_id = 1, /* forward to PF1 */
		};

		struct doca_flow_external_action_timer_entry timer_entry = {
			.resource_offset = 0,
			.countdown_shift = (i == 0) ? TIMER_COUNTDOWN_SHIFT : 0,
		};

		struct doca_flow_ordered_list_element element = {
			.type = DOCA_FLOW_ORDERED_LIST_ELEMENT_EXTERNAL_ACTIONS,
			.external_actions = &timer_entry,
		};
		struct doca_flow_ordered_list ordered_list = {
			.idx = i,
			.size = 1,
			.elements = &element,
		};

		memset(status, 0, sizeof(*status));

		result = doca_flow_pipe_ordered_list_add_entry(0,
							       pipe,
							       i,
							       &ordered_list,
							       &fwd,
							       DOCA_FLOW_ENTRY_FLAGS_NO_WAIT,
							       status,
							       NULL);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to add timer entry %u: %s", i, doca_error_get_descr(result));
			return result;
		}

		result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 1);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process timer entry %u: %s", i, doca_error_get_descr(result));
			return result;
		}

		if (status->nb_processed != 1 || status->failure) {
			DOCA_LOG_ERR("Timer entry %u processing failed", i);
			return DOCA_ERROR_BAD_STATE;
		}
	}

	return DOCA_SUCCESS;
}

/*
 * Sample main logic: demonstrates the external action timer retransmission use case.
 *
 * After creating pipes and entries, the sample polls the timer event queue across the whole
 * traffic window, allowing a traffic generator to inject a START packet (src=1.2.3.4,
 * dst=8.8.8.8) into PF0. If no ACK packet (reversed IPs) arrives before the timer expires,
 * the timer fires an expiry event which the poll loop observes regardless of arrival timing.
 *
 * @nb_queues [in]: number of queues the sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise.
 */
doca_error_t flow_external_action_timer(int nb_queues)
{
	struct flow_resources resource = {0};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_port *ports[NB_PORTS];
	struct doca_dev *dev_arr[NB_PORTS];
	uint32_t actions_mem_size[NB_PORTS];
	struct doca_flow_pipe *timer_pipe = NULL;
	struct doca_flow_pipe *basic_pipe = NULL;
	struct doca_flow_external_resource_timer *timer_resource = NULL;
	void *timer_pipe_actions[2] = {NULL, NULL};
	uint16_t event_queues[] = {0};
	struct doca_flow_external_resource_timer_cfg timer_cfg = {
		.num_of_timers = NR_TIMERS,
		.nr_event_queues = NR_EVENT_QUEUES,
		.event_queue_id = event_queues,
	};
	struct entries_status status = {0};
	int num_of_entries = 4;
	int i;
	int iter;
	doca_error_t result;
	doca_error_t event_poll_err = DOCA_SUCCESS;

	/* Register the timer external action type before DOCA Flow init */
	result = doca_flow_external_action_timer_register();
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to register timer external action: %s", doca_error_get_descr(result));
		return result;
	}

	resource.mode = DOCA_FLOW_RESOURCE_MODE_PORT;

	result = init_doca_flow(nb_queues, "vnf", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(num_of_entries));

	/* Use custom port config to inject timer event queue setup on port 0 */
	memset(dev_arr, 0, sizeof(dev_arr));
	for (i = 0; i < NB_PORTS; i++) {
		result = doca_dpdk_port_as_dev(i, &dev_arr[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get device for port %d: %s", i, doca_error_get_descr(result));
			destroy_doca_flow();
			return result;
		}
	}

	result = init_doca_flow_ports_with_custom_config(NB_PORTS,
							 ports,
							 true,
							 dev_arr,
							 NULL,
							 timer_port_config_cb,
							 NULL,
							 ports,
							 actions_mem_size,
							 &resource,
							 NULL /* no CT */);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		destroy_doca_flow();
		return result;
	}

	/* Create the timer resource on port 0 */
	result = doca_flow_external_resource_timer_create(&timer_cfg, ports[0], &timer_resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create timer resource: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/* Create the ordered-list timer pipe (2 lists: START + STOP) */
	result = create_timer_pipe(ports[0], timer_resource, &timer_pipe, timer_pipe_actions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create timer pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/* Create the basic pipe that steers traffic by src/dst IP */
	result = create_basic_pipe(ports[0], timer_pipe, &basic_pipe, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create basic pipe: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	/* Add timer entries (both START and STOP share the same timer slot) */
	result = add_timer_entries(ports[0], timer_pipe, &status);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add timer entries: %s", doca_error_get_descr(result));
		goto cleanup;
	}

	DOCA_LOG_INFO("Pipes and entries created. Polling ~%d seconds for traffic and timer expiry events.",
		      TRAFFIC_WAIT_SEC + TIMER_EXPIRY_MARGIN_SEC);
	DOCA_LOG_INFO("Send a START packet (src=1.2.3.4, dst=8.8.8.8) and an ACK with swapped IPs to stop the timer.");
	DOCA_LOG_INFO("If an ACK arrives before the timer expires, no expiry event is expected.");
	DOCA_LOG_INFO("If no ACK is sent, a timer expiry message will be printed.");

	for (iter = 0; iter < EVENT_POLL_ITERATIONS; iter++) {
		uint16_t q;

		for (q = 0; q < NR_EVENT_QUEUES; q++) {
			doca_error_t poll_result = doca_flow_event_process(ports[0], DOCA_FLOW_EVENT_TYPE_TIMER, q, 0);

			if (poll_result != DOCA_SUCCESS) {
				DOCA_LOG_WARN("Event process failed on queue %u: %s",
					      q,
					      doca_error_get_descr(poll_result));
				if (event_poll_err == DOCA_SUCCESS)
					event_poll_err = poll_result;
			}
		}

		usleep(EVENT_POLL_SLEEP_US);
	}

	DOCA_LOG_INFO("Event polling complete");

cleanup:
	/* Destroy in reverse dependency order: pipes -> pipe actions -> timer resource -> port stop.
	 * The timer resource must be destroyed while its owning port is still running.
	 */
	doca_flow_pipe_destroy(basic_pipe);
	doca_flow_pipe_destroy(timer_pipe);
	for (iter = 0; iter < 2; iter++) {
		if (timer_pipe_actions[iter])
			doca_flow_external_action_timer_pipe_action_destroy(timer_pipe_actions[iter]);
	}
	doca_flow_external_resource_timer_destroy(timer_resource);
	stop_doca_flow_ports(NB_PORTS, ports);
	destroy_doca_flow();

	if (result == DOCA_SUCCESS && event_poll_err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Event polling encountered errors: %s", doca_error_get_descr(event_poll_err));
		result = event_poll_err;
	}

	return result;
}
