/*
 * Copyright (c) 2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#define _GNU_SOURCE 1
#include <pthread.h>
#include <sched.h>
#include <doca_pe.h>
#include <doca_log.h>

#include <virtiofs_thread.h>
#include <virtiofs_core.h>

DOCA_LOG_REGISTER(VIRTIOFS_THREAD)

#define VIRTIOFS_THREAD_MAX_EVENTS_POLL 16

static inline uint64_t cycle_count_get(void)
{
	uint64_t value = 0;

#ifdef DOCA_ARCH_DPU
	asm volatile("mrs %0, cntvct_el0" : "=r"(value));
#endif
	return value;
}

static inline uint64_t frequency_get(void)
{
	uint64_t freq = 0;

#ifdef DOCA_ARCH_DPU
	asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
#endif
	return freq;
}

static inline uint64_t timer_ms_get(void)
{
	uint64_t cycles = cycle_count_get();
	uint64_t freq = frequency_get();

	return (cycles * 1000) / freq;
}

static void *virtiofs_thread_progress(void *args)
{
	struct virtiofs_thread *thread = (struct virtiofs_thread *)args;
	struct virtiofs_thread_poller *poller;
	int core_id = thread->attr.core_id;
	int event_count;

	DOCA_LOG_INFO("Thread for core %d is running", core_id);

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);

	if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
		perror("pthread_setaffinity_np");

	while (!thread->stop) {
		while (thread->suspend)
			;

		pthread_mutex_lock(&thread->lock);

		if (doca_unlikely(thread->fn)) {
			thread->fn(thread, thread->cb_arg);
			thread->fn = NULL;
			thread->cb_arg = NULL;
		}

		event_count = 0;
		while (thread->curr_inflights < thread->attr.max_inflights && doca_pe_progress(thread->io_pe) &&
		       event_count++ < thread->attr.ctx->num_devices)
			;

		event_count = 0;
		while (doca_pe_progress(thread->dma_pe) && event_count++ < thread->attr.ctx->num_devices)
			;

		if (thread->admin_pe && thread->next_admin_poll < timer_ms_get()) {
			event_count = 0;
			while (doca_pe_progress(thread->admin_pe) && event_count++ < VIRTIOFS_THREAD_MAX_EVENTS_POLL)
				;
			thread->next_admin_poll = timer_ms_get() + 10;
		}

		SLIST_FOREACH(poller, &thread->pollers, entry)
		poller->fn(poller->arg);

		pthread_mutex_unlock(&thread->lock);
	}

	DOCA_LOG_INFO("Thread for core %d exiting.", core_id);
	return NULL;
}

doca_error_t virtiofs_thread_create(struct virtiofs_thread_attr *attr, struct virtiofs_thread **thread)
{
	struct virtiofs_thread *thr;
	pthread_mutexattr_t mutex_attr;
	doca_error_t err;

	thr = calloc(1, sizeof(*thr));
	if (thr == NULL) {
		DOCA_LOG_ERR("Failed to allocate thread for core %d", attr->core_id);
		return DOCA_ERROR_NO_MEMORY;
	}

	thr->attr = *attr;

	if (attr->admin_thread) {
		err = doca_pe_create(&thr->admin_pe);
		if (err != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create admin doca pe on thread %d", attr->core_id);
			goto free_thread;
		}
	}

	err = doca_pe_create(&thr->io_pe);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create io doca pe on thread %d", attr->core_id);
		goto free_admin_pe;
	}

	err = doca_pe_create(&thr->dma_pe);
	if (err != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create dma doca pe on thread %d", attr->core_id);
		goto free_io_pe;
	}

	pthread_mutexattr_init(&mutex_attr);
	pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&thr->lock, &mutex_attr);
	pthread_mutexattr_destroy(&mutex_attr);

	SLIST_INIT(&thr->pollers);
	*thread = thr;

	DOCA_LOG_INFO("Thread created for core %d", attr->core_id);

	return DOCA_SUCCESS;

free_io_pe:
	doca_pe_destroy(thr->io_pe);
free_admin_pe:
	if (thr->admin_pe)
		doca_pe_destroy(thr->admin_pe);
free_thread:
	free(thr);
	return err;
}

doca_error_t virtiofs_thread_start(struct virtiofs_thread *thread)
{
	if (pthread_create(&thread->pthread, NULL, virtiofs_thread_progress, thread) != 0) {
		perror("Failed to create thread");
		return DOCA_ERROR_UNKNOWN;
	}

	return DOCA_SUCCESS;
}

doca_error_t virtiofs_thread_exec(struct virtiofs_thread *thread,
				  virtiofs_thread_exec_fn_t fn,
				  void *arg,
				  bool exec_on_caller_thread)
{
	doca_error_t err;

	thread->suspend = 1;
	pthread_mutex_lock(&thread->lock);
	if (exec_on_caller_thread) {
		/* fn will be executed on current thread */
		err = fn(thread, arg);
	} else {
		/* fn will be executed by virtiofs_thread_progress() on "thread" */
		thread->fn = fn;
		thread->cb_arg = arg;
		err = 0;
	}
	pthread_mutex_unlock(&thread->lock);
	thread->suspend = 0;

	return err;
}

static doca_error_t virtiofs_thread_poller_add_impl(struct virtiofs_thread *thread, void *cb_arg)
{
	struct virtiofs_thread_poller *poller = (struct virtiofs_thread_poller *)cb_arg;

	SLIST_INSERT_HEAD(&thread->pollers, poller, entry);

	return DOCA_SUCCESS;
}

doca_error_t virtiofs_thread_poller_add(struct virtiofs_thread *thread, virtiofs_thread_poller_fn fn, void *arg)
{
	struct virtiofs_thread_poller *poller;

	poller = calloc(1, sizeof(*poller));
	if (poller == NULL)
		return DOCA_ERROR_NO_MEMORY;

	poller->fn = fn;
	poller->arg = arg;

	return virtiofs_thread_exec(thread, virtiofs_thread_poller_add_impl, poller, true);
}

static doca_error_t virtiofs_thread_poller_remove_impl(struct virtiofs_thread *thread, void *cb_arg)
{
	struct virtiofs_thread_poller *poller = (struct virtiofs_thread_poller *)cb_arg;
	struct virtiofs_thread_poller *tmp = NULL;

	SLIST_FOREACH(tmp, &thread->pollers, entry)
	{
		if (tmp->fn == poller->fn && tmp->arg == poller->arg) {
			SLIST_REMOVE(&thread->pollers, tmp, virtiofs_thread_poller, entry);
			free(tmp);
			return DOCA_SUCCESS;
		}
	}

	return DOCA_ERROR_NOT_FOUND;
}

doca_error_t virtiofs_thread_poller_remove(struct virtiofs_thread *thread, virtiofs_thread_poller_fn fn, void *arg)
{
	struct virtiofs_thread_poller poller = {.fn = fn, .arg = arg};

	return virtiofs_thread_exec(thread, virtiofs_thread_poller_remove_impl, &poller, true);
}

struct virtiofs_thread *virtiofs_thread_get(struct virtiofs_resources *ctx)
{
	for (int i = 0; i < ctx->num_threads; i++)
		if (pthread_equal(ctx->threads[i].thread->pthread, pthread_self()))
			return ctx->threads[i].thread;

	return NULL;
}

void virtiofs_thread_stop(struct virtiofs_thread *thread)
{
	thread->stop = 1;
	pthread_join(thread->pthread, NULL);
}

void virtiofs_thread_destroy(struct virtiofs_thread *thread)
{
	/* Check if there are any remaining pollers and warn */
	if (!SLIST_EMPTY(&thread->pollers))
		DOCA_LOG_WARN("Thread being destroyed with active pollers");

	if (thread->admin_pe)
		doca_pe_destroy(thread->admin_pe);
	if (thread->io_pe)
		doca_pe_destroy(thread->io_pe);
	if (thread->dma_pe)
		doca_pe_destroy(thread->dma_pe);

	pthread_mutex_destroy(&thread->lock);
	free(thread);
}
