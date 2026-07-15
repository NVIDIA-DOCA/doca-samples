/*
 * Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <doca_flow.h>
#include <doca_log.h>

#include "simple_fwd.h"
#include "simple_fwd_ft.h"

DOCA_LOG_REGISTER(SIMPLE_FWD_FT);

/* Bucket is a struct encomassing the list and the synchronization mechanism used for accessing the flows list */
struct simple_fwd_ft_bucket {
	struct simple_fwd_ft_entry_head head; /* The head of the list of the flows */
	rte_spinlock_t lock;		      /* Lock, a synchronization mechanism */
};

/* Stats for the flow table */
struct simple_fwd_ft_stats {
	uint64_t add;	 /* Number of insertions to the flow table */
	uint64_t rm;	 /* Number of removals from the flow table */
	uint64_t memuse; /* Memory ysage of the flow table */
};

/* Flow table configuration */
struct simple_fwd_ft_cfg {
	uint32_t size;		 /* Number of maximum flows in a given time while the application is running */
	uint32_t mask;		 /* Masking; */
	uint32_t user_data_size; /* User data size needed for allocation */
	uint32_t entry_size;	 /* Size needed for storing a single entry flow */
};

/* Flow table as represented in the application */
struct simple_fwd_ft {
	struct simple_fwd_ft_cfg cfg;	  /* Flow table configurations */
	struct simple_fwd_ft_stats stats; /* Stats for the flow table */
	bool has_age_thread;		  /* Whether or not a dedicated thread is used */
	pthread_t age_thread;		  /* Thread entity for aging, in case "aging thread" is used */
	volatile int stop_aging_thread;	  /* Flag for stopping the aging thread */
	uint32_t fid_ctr;		  /* Flow table ID , used for controlling the flow table */
	void (*simple_fwd_aging_cb)(struct simple_fwd_ft_user_ctx *ctx); /* Callback holder; callback for handling aged
									    flows */
	void (*simple_fwd_aging_hw_cb)(void);	/* HW callback holder; callback for handling aged flows*/
	struct simple_fwd_ft_bucket buckets[0]; /* Pointer for the Bucket in the flow table; list of entries */
};

void simple_fwd_ft_update_age_sec(struct simple_fwd_ft_entry *e, uint32_t age_sec)
{
	e->age_sec = age_sec;
}

void simple_fwd_ft_update_expiration(struct simple_fwd_ft_entry *e)
{
	if (e->age_sec)
		e->expiration = rte_rdtsc() + rte_get_timer_hz() * e->age_sec;
}

/*
 * Update a counter of a given entry
 *
 * @e [in]: flow entry representation in the application
 * @return: true on success, false otherwise
 */
static bool simple_fwd_ft_update_counter(struct simple_fwd_ft_entry *e)
{
	struct simple_fwd_pipe_entry *entry = (struct simple_fwd_pipe_entry *)&e->user_ctx.data[0];
	struct doca_flow_resource_query query_stats = {0};
	bool update = 0;

	if (doca_flow_resource_query_entry(entry->hw_entry, &query_stats) == DOCA_SUCCESS) {
		update = !!(query_stats.counter.total_pkts - e->last_counter);
		e->last_counter = query_stats.counter.total_pkts;
	}
	return update;
}

/*
 * Destroy flow entry in the flow table
 *
 * @ft [in]: the flow table to remove the entry from
 * @ft_entry [in]: entry flow to remove, as represented in the application
 */
static void _ft_destroy_entry(struct simple_fwd_ft *ft, struct simple_fwd_ft_entry *ft_entry)
{
	LIST_REMOVE(ft_entry, next);
	ft->simple_fwd_aging_cb(&ft_entry->user_ctx);
	free(ft_entry);
	ft->stats.rm++;
}

void simple_fwd_ft_destroy_entry(struct simple_fwd_ft *ft, struct simple_fwd_ft_entry *ft_entry)
{
	int idx = ft_entry->buckets_index;

	rte_spinlock_lock(&ft->buckets[idx].lock);
	_ft_destroy_entry(ft, ft_entry);
	rte_spinlock_unlock(&ft->buckets[idx].lock);
}

/*
 * Start aging handling for a given flow table
 *
 * @ft [in]: the flow table to start the aging handling for
 * @i [in]: the index of the bucket
 * @return: true on success, false when aging handler still not finished all flows in the table
 */
static bool simple_fwd_ft_aging_ft_entry(struct simple_fwd_ft *ft, unsigned int i)
{
	struct simple_fwd_ft_entry *node, *ptr;
	bool still_aging = false;
	uint64_t t = rte_rdtsc();

	if (rte_spinlock_trylock(&ft->buckets[i].lock)) {
		node = LIST_FIRST(&ft->buckets[i].head);
		while (node) {
			ptr = LIST_NEXT(node, next);
			if (node->age_sec && node->expiration < t && !simple_fwd_ft_update_counter(node)) {
				DOCA_LOG_DBG("Aging removing flow");
				_ft_destroy_entry(ft, node);
				still_aging = true;
				break;
			}
			node = ptr;
		}
		rte_spinlock_unlock(&ft->buckets[i].lock);
	}
	return still_aging;
}

/*
 * Main function for aging handler
 *
 * @void_ptr [in]: the flow table to start the aging for
 * @return: Next flow table to handle the aging for, NULL value otherwise
 */
static void *simple_fwd_ft_aging_main(void *void_ptr)
{
	struct simple_fwd_ft *ft = (struct simple_fwd_ft *)void_ptr;
	bool next = false;
	unsigned int i;

	if (!ft) {
		DOCA_LOG_ERR("No ft, abort aging");
		return NULL;
	}
	while (!ft->stop_aging_thread) {
		if ((int)(ft->stats.add - ft->stats.rm) == 0)
			continue;
		DOCA_LOG_DBG("Total entries: %d", (int)(ft->stats.add - ft->stats.rm));
		DOCA_LOG_DBG("Total adds   : %d", (int)(ft->stats.add));
		for (i = 0; i < ft->cfg.size; i++) {
			do {
				next = simple_fwd_ft_aging_ft_entry(ft, i);
			} while (next);
		}
		sleep(1);
	}
	return NULL;
}

/*
 * Start per flow table aging thread
 *
 * @ft [in]: the flow table to start the aging thread for
 * @thread_id [in]: the dedicated thread identifier for aging handling of the provided flow table
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_ft_aging_thread_start(struct simple_fwd_ft *ft, pthread_t *thread_id)
{
	int ret;

	/* create a second thread which executes inc_x(&x) */
	ret = pthread_create(thread_id, NULL, simple_fwd_ft_aging_main, ft);
	if (ret) {
		fprintf(stderr, "Error creating thread ret:%d\n", ret);
		return -1;
	}
	return 0;
}

/*
 * Build table key according to parsed packet.
 *
 * @pinfo [in]: the packet's info
 * @key [out]: the generated key
 * @return: 0 on success and negative value otherwise
 */
static int simple_fwd_ft_key_fill(struct simple_fwd_pkt_info *pinfo, struct simple_fwd_ft_key *key)
{
	bool inner = false;

	if (pinfo->tun_type != DOCA_FLOW_TUN_NONE)
		inner = true;

	/* support ipv6 */
	if (pinfo->outer.l3_type != IPV4)
		return -1;

	key->rss_hash = pinfo->rss_hash;
	/* 5-tuple of inner if there is tunnel or outer if none */
	key->protocol = inner ? pinfo->inner.l4_type : pinfo->outer.l4_type;
	key->ipv4_1 = simple_fwd_ft_key_get_ipv4_src(inner, pinfo);
	key->ipv4_2 = simple_fwd_ft_key_get_ipv4_dst(inner, pinfo);
	key->port_1 = simple_fwd_ft_key_get_src_port(inner, pinfo);
	key->port_2 = simple_fwd_ft_key_get_dst_port(inner, pinfo);
	key->port_id = pinfo->orig_port_id;

	key->tun_type = pinfo->tun_type;
	switch (pinfo->tun_type) {
	case DOCA_FLOW_TUN_VXLAN:
		key->vni = pinfo->tun.vni;
		break;
	case DOCA_FLOW_TUN_GTPU:
		key->teid = pinfo->tun.teid;
		break;
	case DOCA_FLOW_TUN_GRE:
		key->gre_key = pinfo->tun.gre_key;
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Compare keys
 *
 * @key1 [in]: first key for comparison
 * @key2 [in]: first key for comparison
 * @return: true if keys are equal, false otherwise
 */
static bool simple_fwd_ft_key_equal(struct simple_fwd_ft_key *key1, struct simple_fwd_ft_key *key2)
{
	return (memcmp(key1, key2, sizeof(struct simple_fwd_ft_key)) == 0);
}

struct simple_fwd_ft *simple_fwd_ft_create(int nb_flows,
					   uint32_t user_data_size,
					   void (*simple_fwd_aging_cb)(struct simple_fwd_ft_user_ctx *ctx),
					   void (*simple_fwd_aging_hw_cb)(void),
					   bool age_thread)
{
	struct simple_fwd_ft *ft;
	uint32_t nb_flows_aligned;
	uint32_t alloc_size;
	uint32_t i;

	if (nb_flows <= 0)
		return NULL;
	/* Align to the next power of 2, 32bits integer is enough now */
	if (!rte_is_power_of_2(nb_flows))
		nb_flows_aligned = rte_align32pow2(nb_flows);
	else
		nb_flows_aligned = nb_flows;
	/* double the flows to avoid collisions */
	nb_flows_aligned <<= 1;
	alloc_size = sizeof(struct simple_fwd_ft) + sizeof(struct simple_fwd_ft_bucket) * nb_flows_aligned;
	DOCA_LOG_TRC("Malloc size =%d", alloc_size);

	ft = calloc(1, alloc_size);
	if (ft == NULL) {
		DOCA_LOG_ERR("No memory");
		return NULL;
	}
	ft->cfg.entry_size = sizeof(struct simple_fwd_ft_entry) + user_data_size;
	ft->cfg.user_data_size = user_data_size;
	ft->cfg.size = nb_flows_aligned;
	ft->cfg.mask = nb_flows_aligned - 1;
	ft->simple_fwd_aging_cb = simple_fwd_aging_cb;
	ft->simple_fwd_aging_hw_cb = simple_fwd_aging_hw_cb;

	DOCA_LOG_TRC("FT created: flows=%d, user_data_size=%d", nb_flows_aligned, user_data_size);
	for (i = 0; i < ft->cfg.size; i++)
		rte_spinlock_init(&ft->buckets[i].lock);
	if (age_thread && simple_fwd_ft_aging_thread_start(ft, &ft->age_thread) < 0) {
		free(ft);
		return NULL;
	}
	ft->has_age_thread = age_thread;
	return ft;
}

/*
 * find if there is an existing entry matching the given packet generated key
 *
 * @ft [in]: flow table to search in
 * @key [in]: the packet generated key used for search in the flow table
 * @return: pointer to the flow entry if found, NULL otherwise
 */
static struct simple_fwd_ft_entry *_simple_fwd_ft_find(struct simple_fwd_ft *ft, struct simple_fwd_ft_key *key)
{
	uint32_t idx;
	struct simple_fwd_ft_entry_head *first;
	struct simple_fwd_ft_entry *node;

	idx = key->rss_hash & ft->cfg.mask;
	DOCA_LOG_TRC("Looking for index %d", idx);
	first = &ft->buckets[idx].head;
	LIST_FOREACH(node, first, next)
	{
		if (simple_fwd_ft_key_equal(&node->key, key)) {
			simple_fwd_ft_update_expiration(node);
			return node;
		}
	}
	return NULL;
}

doca_error_t simple_fwd_ft_find(struct simple_fwd_ft *ft,
				struct simple_fwd_pkt_info *pinfo,
				struct simple_fwd_ft_user_ctx **ctx)
{
	doca_error_t result = DOCA_SUCCESS;
	struct simple_fwd_ft_entry *fe;
	struct simple_fwd_ft_key key = {0};

	if (simple_fwd_ft_key_fill(pinfo, &key)) {
		result = DOCA_ERROR_UNEXPECTED;
		DOCA_LOG_DBG("Failed to build key for entry in the flow table %s", doca_error_get_descr(result));
		return result;
	}

	fe = _simple_fwd_ft_find(ft, &key);
	if (fe == NULL) {
		result = DOCA_ERROR_NOT_FOUND;
		DOCA_LOG_DBG("Entry not found in flow table %s", doca_error_get_descr(result));
		return result;
	}

	*ctx = &fe->user_ctx;
	return DOCA_SUCCESS;
}

doca_error_t simple_fwd_ft_add_new(struct simple_fwd_ft *ft,
				   struct simple_fwd_pkt_info *pinfo,
				   struct simple_fwd_ft_user_ctx **ctx)
{
	doca_error_t result = DOCA_SUCCESS;
	int idx;
	struct simple_fwd_ft_key key = {0};
	struct simple_fwd_ft_entry *new_e;
	struct simple_fwd_ft_entry_head *first;

	if (!ft)
		return false;

	if (simple_fwd_ft_key_fill(pinfo, &key)) {
		result = DOCA_ERROR_UNEXPECTED;
		DOCA_LOG_DBG("Failed to build key: %s", doca_error_get_descr(result));
		return result;
	}

	new_e = calloc(1, ft->cfg.entry_size);
	if (new_e == NULL) {
		result = DOCA_ERROR_NO_MEMORY;
		DOCA_LOG_WARN("OOM: %s", doca_error_get_descr(result));
		return result;
	}

	simple_fwd_ft_update_expiration(new_e);
	new_e->user_ctx.fid = ft->fid_ctr++;
	*ctx = &new_e->user_ctx;

	DOCA_LOG_TRC("Defined new flow %llu", (unsigned int long long)new_e->user_ctx.fid);
	memcpy(&new_e->key, &key, sizeof(struct simple_fwd_ft_key));
	idx = pinfo->rss_hash & ft->cfg.mask;
	new_e->buckets_index = idx;
	first = &ft->buckets[idx].head;

	rte_spinlock_lock(&ft->buckets[idx].lock);
	LIST_INSERT_HEAD(first, new_e, next);
	rte_spinlock_unlock(&ft->buckets[idx].lock);
	ft->stats.add++;
	return result;
}

doca_error_t simple_fwd_ft_destroy(struct simple_fwd_ft *ft)
{
	uint32_t i;
	struct simple_fwd_ft_entry *node, *ptr;

	if (ft == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (ft->has_age_thread) {
		ft->stop_aging_thread = true;
		pthread_join(ft->age_thread, NULL);
	}
	for (i = 0; i < ft->cfg.size; i++) {
		node = LIST_FIRST(&ft->buckets[i].head);
		while (node != NULL) {
			ptr = LIST_NEXT(node, next);
			_ft_destroy_entry(ft, node);
			node = ptr;
		}
	}
	free(ft);
	return DOCA_SUCCESS;
}
