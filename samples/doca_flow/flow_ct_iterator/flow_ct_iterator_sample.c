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

#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>

#include <rte_ethdev.h>

#include <doca_log.h>
#include <doca_flow.h>
#include <doca_flow_ct.h>

#include "flow_ct_common.h"
#include <flow_common.h>
#include "flow_switch_common.h"

DOCA_LOG_REGISTER(FLOW_CT_ITERATOR);

/* Create a pipe and an entry that match any packets and froward to fwd */
static doca_error_t create_pipe_and_entry(struct doca_flow_port *port,
					  struct entries_status *status,
					  const char *pipe_name,
					  struct doca_flow_fwd *fwd,
					  struct doca_flow_pipe **pipe,
					  struct doca_flow_pipe_entry **entry)
{
	struct doca_flow_match match = {};
	struct doca_flow_monitor monitor = {.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED};
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, pipe_name, DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg monitor: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(cfg, fwd, NULL, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RSS pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	doca_flow_pipe_cfg_destroy(cfg);

	/* Match on any packet */
	result = doca_flow_pipe_add_entry(0, *pipe, NULL, 0, NULL, &monitor, NULL, 0, status, entry);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add RSS pipe entry: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process RSS entry: %s", doca_error_get_descr(result));

	return result;

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

/* Create empty pipe with DROP miss and counter */
static doca_error_t create_dummy_pipe(struct doca_flow_port *port,
				      const char *pipe_name,
				      struct doca_flow_fwd *fwd,
				      struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *cfg;
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_flow_pipe_cfg_set_miss_counter(cfg, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg miss counter: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = set_flow_pipe_cfg(cfg, pipe_name, DOCA_FLOW_PIPE_BASIC, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(cfg, fwd, fwd, pipe);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create RSS pipe: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_entries_process(port, 0, DEFAULT_TIMEOUT_US, 0);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to process RSS entry: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

static doca_error_t create_ct_pipe(struct doca_flow_port *port,
				   struct doca_flow_pipe *fwd_match_pipe,
				   struct doca_flow_pipe *fwd_miss_pipe,
				   struct doca_flow_pipe **pipe)
{
	struct doca_flow_pipe_cfg *cfg;
	struct doca_flow_match match = {};
	struct doca_flow_fwd fwd = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = fwd_match_pipe};
	struct doca_flow_fwd fwd_miss = {.type = DOCA_FLOW_FWD_PIPE, .next_pipe = fwd_miss_pipe};
	doca_error_t result;

	result = doca_flow_pipe_cfg_create(&cfg, port);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		return result;
	}

	result = set_flow_pipe_cfg(cfg, "CT_PIPE", DOCA_FLOW_PIPE_CT, false);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}
	result = doca_flow_pipe_cfg_set_match(cfg, &match, NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set doca_flow_pipe_cfg match: %s", doca_error_get_descr(result));
		goto destroy_pipe_cfg;
	}

	result = doca_flow_pipe_create(cfg, &fwd, &fwd_miss, pipe);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to add CT pipe: %s", doca_error_get_descr(result));

destroy_pipe_cfg:
	doca_flow_pipe_cfg_destroy(cfg);
	return result;
}

static doca_error_t create_ct_entries(uint32_t n_ct_entries,
				      struct doca_flow_port *port,
				      struct doca_flow_pipe *ct_pipe)
{
	struct doca_flow_ct_match match_origin = {
		.ipv4 = {.next_proto = DOCA_FLOW_PROTO_UDP,
			 .dst_ip = BE_IPV4_ADDR(1, 1, 1, 1),
			 .l4_port = {.src_port = DOCA_HTOBE16(1), .dst_port = DOCA_HTOBE16(1)}}};
	struct doca_flow_ct_match match_reply = {
		.ipv4 = {.next_proto = DOCA_FLOW_PROTO_UDP,
			 .src_ip = BE_IPV4_ADDR(1, 1, 1, 1),
			 .l4_port = {.src_port = DOCA_HTOBE16(1), .dst_port = DOCA_HTOBE16(1)}}};
	struct doca_flow_ct_actions action = {.resource_type = DOCA_FLOW_RESOURCE_TYPE_NONE};
	struct entries_status ct_status = {0};
	struct doca_flow_pipe_entry *entry;
	uint32_t i;
	doca_error_t ret;

	for (i = 0; i < n_ct_entries; i++) {
		match_origin.ipv4.src_ip = DOCA_HTOBE32(i);
		match_reply.ipv4.dst_ip = DOCA_HTOBE32(i);
		ret = flow_ct_create_entry(port,
					   1,
					   ct_pipe,
					   DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS,
					   DOCA_FLOW_CT_ENTRY_FLAGS_NO_WAIT | DOCA_FLOW_CT_ENTRY_FLAGS_DIR_ORIGIN |
						   DOCA_FLOW_CT_ENTRY_FLAGS_DIR_REPLY,
					   &match_origin,
					   &match_reply,
					   i,
					   i,
					   &action,
					   &action,
					   0,
					   0,
					   0,
					   &ct_status,
					   &entry);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create CT entry: %s", doca_error_get_descr(ret));
			return ret;
		}
	}

	DOCA_LOG_INFO("created %d ct entries on port 0", n_ct_entries);

	return DOCA_SUCCESS;
}

struct iterate_usr_ctx {
	struct doca_flow_port *port_dst;
	struct doca_flow_pipe *ct_pipe_src;
	struct doca_flow_pipe *ct_pipe_dst;
	bool iterate_only;
	uint32_t n_entries;
	uint32_t n_entries_total;
	struct entries_status ct_status;
};

/* get entry info from src pipe and create new entry on dst pipe */
void iterate_cb(uint16_t pipe_queue,
		struct doca_flow_pipe *pipe,
		struct doca_flow_pipe_entry *entry,
		void *iterate_usr_ctx)
{
	struct iterate_usr_ctx *ctx = (struct iterate_usr_ctx *)iterate_usr_ctx;
	struct doca_flow_ct_actions action = {.resource_type = DOCA_FLOW_RESOURCE_TYPE_NONE};
	doca_error_t ret;
	struct doca_flow_ct_match match_origin;
	struct doca_flow_ct_match match_reply;
	uint64_t entry_flags;
	uint32_t hash_origin;
	uint32_t hash_reply;

	ret = doca_flow_ct_get_entry(pipe_queue,
				     pipe,
				     0,
				     entry,
				     &match_origin,
				     &match_reply,
				     &entry_flags,
				     &hash_origin,
				     &hash_reply);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get CT entry %d: %s", ctx->n_entries_total, doca_error_get_descr(ret));
		return;
	}

	if (!ctx->iterate_only) {
		ret = flow_ct_create_entry(ctx->port_dst,
					   1,
					   ctx->ct_pipe_dst,
					   DOCA_FLOW_CT_ENTRY_FLAGS_ALLOC_ON_MISS,
					   entry_flags, /* queue requests, wait for 32 entries to be processed */
					   &match_origin,
					   &match_reply,
					   hash_origin,
					   hash_reply,
					   &action,
					   &action,
					   0,
					   0,
					   0,
					   &ctx->ct_status,
					   &entry);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create CT entry: %s", doca_error_get_descr(ret));
			return;
		}
	}

	ctx->n_entries++;
	ctx->n_entries_total++;
}

static doca_error_t ct_pipe_iterate(struct doca_flow_pipe *ct_pipe_src,
				    struct doca_flow_pipe *ct_pipe_dst,
				    struct doca_flow_port *port_src,
				    struct doca_flow_port *port_dst,
				    bool iterate_only)
{
	struct iterate_usr_ctx iterate_usr_ctx = {.port_dst = port_dst,
						  .ct_pipe_src = ct_pipe_src,
						  .ct_pipe_dst = ct_pipe_dst,
						  .iterate_only = iterate_only};
	doca_error_t ret;
	struct timeval start_time, end_time;
	long elapsed_ms;

	gettimeofday(&start_time, NULL);

	doca_flow_ct_pipe_iterate(ct_pipe_src, iterate_cb, &iterate_usr_ctx);
	do {
		iterate_usr_ctx.n_entries = 0;

		/* push and reserve queue room for 64 rule entries on dst port */
		ret = doca_flow_ct_entries_process(port_dst, 1, 64, 0, NULL);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process CT entries: %s on port 1", doca_error_get_descr(ret));
			return ret;
		}

		/* iterate over src pipe */
		ret = doca_flow_ct_entries_process(port_src, 1, 0, 32, NULL);
		if (ret != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to process CT entries: %s on port 0", doca_error_get_descr(ret));
			return ret;
		}
	} while (iterate_usr_ctx.n_entries == 32);

	/* process remaining entries on dst port */
	ret = doca_flow_ct_entries_process(port_dst, 1, 512, 0, NULL);
	if (ret != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to process CT entries: %s on port 1", doca_error_get_descr(ret));
		return ret;
	}

	gettimeofday(&end_time, NULL);
	elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000L + (end_time.tv_usec - start_time.tv_usec) / 1000L;

	DOCA_LOG_INFO("total entries iterated: %d, in %ld ms", iterate_usr_ctx.n_entries_total, elapsed_ms);

	return DOCA_SUCCESS;
}

/* Context structure for statistics printing */
struct ct_iterator_stats_context {
	int nb_ports;
	struct doca_flow_pipe_entry **port_forward_entries;
	struct doca_flow_pipe **miss_drop_pipes;
};

/*
 * Print CT iterator statistics
 *
 * @nb_ports [in]: number of ports
 * @port_forward_entries [in]: array of port forward entries
 * @miss_drop_pipes [in]: array of miss drop pipes
 */
static void print_ct_iterator_stats(int nb_ports,
				    struct doca_flow_pipe_entry *port_forward_entries[],
				    struct doca_flow_pipe *miss_drop_pipes[])
{
	doca_error_t result;
	struct doca_flow_resource_query query_fwd, query_miss;
	int i;

	for (i = 0; i < nb_ports; i++) {
		result = doca_flow_resource_query_entry(port_forward_entries[i], &query_fwd);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query forward pipe counter (port %d): %s",
				     i,
				     doca_error_get_descr(result));
			return;
		}
		result = doca_flow_resource_query_pipe_miss(miss_drop_pipes[i], &query_miss);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to query miss pipe counter (port %d): %s",
				     i,
				     doca_error_get_descr(result));
			return;
		}
		DOCA_LOG_INFO("Port %d, CT forward hit: %ld, miss: %ld",
			      i,
			      query_fwd.counter.total_pkts,
			      query_miss.counter.total_pkts);
	}
}

/*
 * Wrapper function for statistics printing compatible with flow_wait_for_packets
 *
 * @context [in]: ct_iterator_stats_context structure
 */
static void print_ct_iterator_stats_wrapper(void *context)
{
	struct ct_iterator_stats_context *ctx = (struct ct_iterator_stats_context *)context;
	print_ct_iterator_stats(ctx->nb_ports, ctx->port_forward_entries, ctx->miss_drop_pipes);
}

/*
 * Connection Tracking Iterator Sample
 *
 * This sample do the following:
 * 1. init 2 ports
 * 2. create 2 CT pipes, one for each port
 * 3. create N entries on first CT pipe
 * 4. iterate over all entries and create new entries on second CT pipe
 * 5. same packet should be forwarded back to source port on both port.
 */
doca_error_t flow_ct_iterator(uint32_t n_ct_entries, struct flow_dev_ctx *ctx, bool iterate_only)
{
	const int nb_entries = 4;
	int nb_ports = 2;
	struct doca_flow_port *ports[2];
	struct flow_resources resource = {.mode = DOCA_FLOW_RESOURCE_MODE_PORT, .nr_counters = 1024};
	uint32_t nr_shared_resources[SHARED_RESOURCE_NUM_VALUES] = {0};
	struct doca_flow_fwd port_forward_fwd = {.type = DOCA_FLOW_FWD_PORT};
	struct doca_flow_fwd miss_drop_fwd = {.type = DOCA_FLOW_FWD_DROP};
	struct doca_flow_pipe *port_forward_pipes[2] = {NULL, NULL}; /* pipe for match packets -> port fwd */
	struct doca_flow_pipe_entry *port_forward_entries[2] = {NULL, NULL};
	struct doca_flow_pipe *miss_drop_pipes[2] = {NULL, NULL}; /* RSS pipe for miss packets -> software RX queue */
	struct doca_flow_pipe *counter_miss_pipes[2] = {NULL, NULL}; /* Counter pipe for CT miss packets */
	struct doca_flow_pipe *ct_pipes[2] = {NULL, NULL};	     /* Connection tracking pipe */
	struct doca_flow_pipe *root_pipes[2] = {NULL, NULL};	     /* Connection tracking pipe */
	uint32_t actions_mem_size[2];
	struct doca_flow_meta zone_mask = {};
	struct doca_flow_ct_meta modify_mask = {};
	struct entries_status ctrl_status;
	uint32_t ct_flags = DOCA_FLOW_CT_FLAG_NO_AGING | DOCA_FLOW_CT_FLAG_ITERATOR;
	doca_error_t result;
	int i;

	struct ct_iterator_stats_context stats_ctx = {.nb_ports = nb_ports,
						      .port_forward_entries = port_forward_entries,
						      .miss_drop_pipes = miss_drop_pipes};

	if (ctx->nb_devs != 2 || ctx->nb_ports != 2) {
		DOCA_LOG_ERR("This sample requires 2 eswitch master devices");
		return DOCA_ERROR_BAD_STATE;
	}

	result = init_doca_flow(1, "switch,hws,isolated", &resource, nr_shared_resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA Flow: %s", doca_error_get_descr(result));
		return result;
	}

	result = init_doca_flow_ct(ct_flags,
				   1,
				   0,
				   0,
				   NULL,
				   n_ct_entries,
				   0,
				   0,
				   0, /* CT pipe decide it in iterator mode */
				   false,
				   &zone_mask,
				   &modify_mask,
				   false,
				   &zone_mask,
				   &modify_mask);
	if (result != DOCA_SUCCESS) {
		doca_flow_destroy();
		return result;
	}

	ARRAY_INIT(actions_mem_size, ACTIONS_MEM_SIZE(nb_entries));
	result = init_doca_flow_switch_ports(ctx->devs_manager,
					     ctx->nb_devs,
					     ports,
					     nb_ports,
					     actions_mem_size,
					     &resource);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to init DOCA ports: %s", doca_error_get_descr(result));
		doca_flow_ct_destroy();
		doca_flow_destroy();
		return result;
	}

	for (i = 0; i < nb_ports; i++) {
		memset(&ctrl_status, 0, sizeof(ctrl_status));

		result = create_dummy_pipe(ports[i], "MISS_DROP_PIPE", &miss_drop_fwd, &miss_drop_pipes[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		port_forward_fwd.port_id = i;
		result = create_pipe_and_entry(ports[i],
					       &ctrl_status,
					       "PORT_FORWARD_PIPE",
					       &port_forward_fwd,
					       &port_forward_pipes[i],
					       &port_forward_entries[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		/* Create CT pipe with built-in counter: match->port_forward (direct) and miss->counter_miss */
		result = create_ct_pipe(ports[i], port_forward_pipes[i], miss_drop_pipes[i], &ct_pipes[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		result = create_ct_root_pipe(ports[i],
					     true,
					     false,
					     DOCA_FLOW_L4_META_UDP,
					     ct_pipes[i],
					     &ctrl_status,
					     &root_pipes[i]);
		if (result != DOCA_SUCCESS)
			goto cleanup;

		if (ctrl_status.nb_processed != nb_entries || ctrl_status.failure) {
			DOCA_LOG_ERR("Failed to process control path entries");
			result = DOCA_ERROR_BAD_STATE;
			goto cleanup;
		}
	}

	result = create_ct_entries(n_ct_entries, ports[0], ct_pipes[0]);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	result = ct_pipe_iterate(ct_pipes[0], ct_pipes[1], ports[0], ports[1], iterate_only);
	if (result != DOCA_SUCCESS)
		goto cleanup;

	DOCA_LOG_INFO("Wait a few seconds for packets to arrive, please send UDP packets to both ports\n");

	flow_wait_for_packets(5, print_ct_iterator_stats_wrapper, &stats_ctx);

cleanup:
	for (i = 0; i < nb_ports; i++) {
		if (root_pipes[i] != NULL)
			doca_flow_pipe_destroy(root_pipes[i]);
		if (ct_pipes[i] != NULL)
			doca_flow_pipe_destroy(ct_pipes[i]);
		if (port_forward_pipes[i] != NULL)
			doca_flow_pipe_destroy(port_forward_pipes[i]);
		if (counter_miss_pipes[i] != NULL)
			doca_flow_pipe_destroy(counter_miss_pipes[i]);
		if (miss_drop_pipes[i] != NULL)
			doca_flow_pipe_destroy(miss_drop_pipes[i]);
	}
	cleanup_procedure(NULL, nb_ports, ports);
	return result;
}
