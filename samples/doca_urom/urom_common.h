/*
 * Copyright (c) 2023 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef UROM_COMMON_H_
#define UROM_COMMON_H_

#include <doca_dev.h>
#include <doca_urom.h>
#include <doca_pe.h>

/*
 * Struct contains all the common configurations that needed for DOCA UROM samples.
 */
struct urom_common_cfg {
	char device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE]; /* DOCA device name */
};

/*
 * Struct contains domain shared buffer details
 */
struct urom_domain_buffer_attrs {
	void *buffer;	 /* Buffer address */
	size_t buf_len;	 /* Buffer length */
	void *memh;	 /* Buffer packed memory handle */
	size_t memh_len; /* Buffer packed memory handle length */
	void *mkey;	 /* Buffer packed memory key */
	size_t mkey_len; /* Buffer packed memory key length*/
};

/*
 * Register the common command line parameter for the sample.
 *
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t register_urom_common_params(void);

/*
 * Start UROM service context
 *
 * @pe [in]: Progress engine
 * @dev [in]: service DOCA device
 * @nb_workers [in]: number of workers
 * @service [out]: service context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t start_urom_service(struct doca_pe *pe,
				struct doca_dev *dev,
				uint64_t nb_workers,
				struct doca_urom_service **service);

/*
 * Start UROM worker context
 *
 * @pe [in]: Progress engine
 * @service [in]: service context
 * @worker_id [in]: Worker id
 * @gid [in]: worker group id (optional attribute)
 * @nb_tasks [in]: number of tasks
 * @cpuset [in]: worker CPU affinity to set
 * @env [in]: worker environment variables array
 * @env_count [in]: worker environment variables array size
 * @plugins [in]: worker plugins
 * @worker [out]: set worker context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t start_urom_worker(struct doca_pe *pe,
			       struct doca_urom_service *service,
			       uint64_t worker_id,
			       uint32_t *gid,
			       uint64_t nb_tasks,
			       doca_cpu_set_t *cpuset,
			       char **env,
			       size_t env_count,
			       uint64_t plugins,
			       struct doca_urom_worker **worker);

/*
 * Start UROM domain context
 *
 * @pe [in]: Progress engine
 * @oob [in]: OOB allgather operations
 * @worker_ids [in]: workers ids participate in domain
 * @workers [in]: workers participate in domain
 * @nb_workers [in]: number of workers in domain
 * @buffers [in]: shared buffers
 * @nb_buffers [out]: number of shared buffers
 * @domain [out]: domain context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t start_urom_domain(struct doca_pe *pe,
			       struct doca_urom_domain_oob_coll *oob,
			       uint64_t *worker_ids,
			       struct doca_urom_worker **workers,
			       size_t nb_workers,
			       struct urom_domain_buffer_attrs *buffers,
			       size_t nb_buffers,
			       struct doca_urom_domain **domain);
#endif /* UROM_COMMON_H_ */
