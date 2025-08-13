/*
 * Copyright (c) 2022-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <time.h>

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_log.h>

#include "dpa_all_to_all_core.h"

#define MAX_MPI_WAIT_TIME (10)	      /* Maximum time to wait on MPI request */
#define SLEEP_IN_NANO_SEC (100000000) /* Sleeping interval for completion polling */

DOCA_LOG_REGISTER(A2A::Core);

/*
 * A struct that includes all needed info on registered kernels and is initialized during linkage by DPACC.
 * Variable name should be the token passed to DPACC with --app-name parameter.
 */
extern struct doca_dpa_app *dpa_all2all_app;

/* IB devices names */
char pf_device1_name[MAX_IB_DEVICE_NAME_LEN];
char pf_device2_name[MAX_IB_DEVICE_NAME_LEN];
char rdma_device1_name[MAX_IB_DEVICE_NAME_LEN];
char rdma_device2_name[MAX_IB_DEVICE_NAME_LEN];

/* DOCA DPA all to all kernel function pointer */
doca_dpa_func_t alltoall_kernel;

/*
 * Calculate the width of the integers (according to the number of digits)
 * Note that this functions wouldn't work for n = MIN_INT however in the usage of this function here is guaranteed not
 * to use such values.
 *
 * @n [in]: An integer
 * @return: The width of the integer on success and negative value otherwise
 */
static int calc_width(int n)
{
	if (n < 0)
		n = -n;
	if (n < 10)
		return 1;
	return floor(log10(n) + 1);
}

/*
 * Print buffer as a matrix
 *
 * @buff [in]: A buffer of integers
 * @columns [in]: Number of columns
 * @rows [in]: Number of rows
 */
static void print_buff(const int *buff, size_t columns, size_t rows)
{
	int max_wdt1 = 0;
	int max_wdt2 = 0;
	int tmp, wdt, i, j;
	const int *tmp_buff = buff;

	for (i = 0; i < columns * rows; i++) {
		tmp = calc_width(buff[i]);
		max_wdt1 = (tmp > max_wdt1) ? tmp : max_wdt1;
	}
	max_wdt2 = calc_width(rows);
	for (j = 0; j < rows; j++) {
		printf("Rank %d", j);
		wdt = calc_width(j);
		for (; wdt < max_wdt2; wdt++)
			printf(" ");
		printf(" |");
		for (i = 0; i < columns - 1; i++) {
			wdt = calc_width(tmp_buff[i]);
			printf("%d   ", tmp_buff[i]);
			for (; wdt < max_wdt1; wdt++)
				printf(" ");
		}
		printf("%d", tmp_buff[columns - 1]);
		wdt = calc_width(tmp_buff[columns - 1]);
		for (; wdt < max_wdt1; wdt++)
			printf(" ");
		printf("|\n");
		tmp_buff += columns;
	}
}

/*
 * Generate a random integer between 0 and 10000
 *
 * @return: A random integer between 0 and 10000 on success and negative value otherwise
 */
static int compute_random_int(void)
{
	return (rand() % 10000);
}

/*
 * Wait for MPI request to finish or until timeout
 *
 * @req [in]: MPI request
 * @timeout [in]: Maximum time to wait on request, in seconds
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t mpi_request_wait_timeout(MPI_Request *req, size_t timeout)
{
	time_t now = time(NULL);
	int status;

	MPI_Test(req, &status, MPI_STATUS_IGNORE);
	/* Wait until request returns true or timeout */
	while (status == 0 && (time(NULL) < now + timeout))
		MPI_Test(req, &status, MPI_STATUS_IGNORE);

	/* Return success if request finishes and error otherwise */
	if (status)
		return DOCA_SUCCESS;
	else
		return DOCA_ERROR_TIME_OUT;
}

bool dpa_device_exists_check(const char *device_name)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	bool exists = false;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	int i = 0;

	/* If it's the default then return true */
	if (strncmp(device_name, IB_DEVICE_DEFAULT_NAME, strlen(IB_DEVICE_DEFAULT_NAME)) == 0)
		return true;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return false;
	}

	/* Search device with same dev name*/
	for (i = 0; i < nb_devs; i++) {
		result = doca_dpa_cap_is_supported(dev_list[i]);
		if (result != DOCA_SUCCESS)
			continue;

		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS)
			continue;

		/* Check if we found the device with the wanted name */
		if (strncmp(device_name, ibdev_name, MAX_IB_DEVICE_NAME_LEN) == 0) {
			exists = true;
			break;
		}
	}

	doca_devinfo_destroy_list(dev_list);

	return exists;
}

bool rdma_device_exists_check(const char *device_name)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result;
	bool exists = false;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	int i = 0;

	/* If it's the default then return true */
	if (strncmp(device_name, IB_DEVICE_DEFAULT_NAME, strlen(IB_DEVICE_DEFAULT_NAME)) == 0)
		return true;

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return false;
	}

	/* Search device with same dev name*/
	for (i = 0; i < nb_devs; i++) {
		result = doca_rdma_cap_task_send_is_supported(dev_list[i]);
		if (result != DOCA_SUCCESS)
			continue;

		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS)
			continue;

		/* Check if we found the device with the wanted name */
		if (strncmp(device_name, ibdev_name, MAX_IB_DEVICE_NAME_LEN) == 0) {
			exists = true;
			break;
		}
	}

	doca_devinfo_destroy_list(dev_list);

	return exists;
}

/*
 * Open DPA DOCA devices
 *
 * When running from DPU, rdma_doca_device will be opened for SF DOCA device with RDMA capability.
 * When running from Host, returned rdma_doca_device is equal to pf_doca_device.
 *
 * @pf_device_name [in]: Wanted PF device name, can be NOT_SET and then a random DPA supported device is chosen
 * @rdma_device_name [in]: Relevant when running from DPU. Wanted RDMA device name, can be NOT_SET and then a random
 * RDMA supported device is chosen
 * @pf_doca_device [out]: An allocated PF DOCA device on success and NULL otherwise
 * @rdma_doca_device [out]: An allocated RDMA DOCA device on success and NULL otherwise
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t open_dpa_devices(const char *pf_device_name,
				     const char *rdma_device_name,
				     struct doca_dev **pf_doca_device,
				     struct doca_dev **rdma_doca_device)
{
	struct doca_devinfo **dev_list;
	uint32_t nb_devs = 0;
	doca_error_t result, dpa_cap;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	char actual_base_ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	uint32_t i = 0;

	if (strcmp(pf_device_name, IB_DEVICE_DEFAULT_NAME) != 0 &&
	    strcmp(rdma_device_name, IB_DEVICE_DEFAULT_NAME) != 0 && strcmp(pf_device_name, rdma_device_name) == 0) {
		DOCA_LOG_ERR("RDMA DOCA device must be different than PF DOCA device (%s)", pf_device_name);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to load DOCA devices list: %s", doca_error_get_descr(result));
		return result;
	}

	for (i = 0; i < nb_devs; i++) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS) {
			continue;
		}

#ifdef DOCA_ARCH_DPU
		doca_error_t rdma_cap = doca_rdma_cap_task_send_is_supported(dev_list[i]);
		if (*rdma_doca_device == NULL && rdma_cap == DOCA_SUCCESS) {
			/* to be able to extend rdma device later on (if needed), it must be a different device */
			if (strcmp(ibdev_name, actual_base_ibdev_name) == 0) {
				continue;
			}
			if (strncmp(rdma_device_name, IB_DEVICE_DEFAULT_NAME, strlen(IB_DEVICE_DEFAULT_NAME)) == 0 ||
			    strncmp(rdma_device_name, ibdev_name, MAX_IB_DEVICE_NAME_LEN) == 0) {
				result = doca_dev_open(dev_list[i], rdma_doca_device);
				if (result != DOCA_SUCCESS) {
					doca_devinfo_destroy_list(dev_list);
					DOCA_LOG_ERR("Failed to open DOCA device %s: %s",
						     ibdev_name,
						     doca_error_get_descr(result));
					return result;
				}
			}
		}
#endif

		dpa_cap = doca_dpa_cap_is_supported(dev_list[i]);
		if (*pf_doca_device == NULL && dpa_cap == DOCA_SUCCESS) {
			if (strncmp(pf_device_name, IB_DEVICE_DEFAULT_NAME, strlen(IB_DEVICE_DEFAULT_NAME)) == 0 ||
			    strncmp(pf_device_name, ibdev_name, MAX_IB_DEVICE_NAME_LEN) == 0) {
				result = doca_dev_open(dev_list[i], pf_doca_device);
				if (result != DOCA_SUCCESS) {
					doca_devinfo_destroy_list(dev_list);
					DOCA_LOG_ERR("Failed to open DOCA device %s: %s",
						     ibdev_name,
						     doca_error_get_descr(result));
					return result;
				}
				strncpy(actual_base_ibdev_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
			}
		}
	}

	doca_devinfo_destroy_list(dev_list);

	if (*pf_doca_device == NULL) {
		DOCA_LOG_ERR("Couldn't get PF DOCA device");
		return DOCA_ERROR_NOT_FOUND;
	}

#ifdef DOCA_ARCH_DPU
	if (*rdma_doca_device == NULL) {
		DOCA_LOG_ERR("Couldn't get RDMA DOCA device");
		return DOCA_ERROR_NOT_FOUND;
	}
#else
	*rdma_doca_device = *pf_doca_device;
#endif

	return result;
}

/*
 * Create DOCA DPA context
 *
 * @resources [in/out]: All to all resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_dpa_context(struct a2a_resources *resources)
{
	doca_error_t result, tmp_result;

	/* Open doca devices */
	result = open_dpa_devices(resources->pf_device_name,
				  resources->rdma_device_name,
				  &(resources->pf_doca_device),
				  &(resources->rdma_doca_device));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("open_dpa_devices() failed");
		return result;
	}

	/* Sleep for 10 * my_rank milliseconds to ensure resources are ready to use before creating the context */
	struct timespec sleep_ts = {
		.tv_sec = 0,
		.tv_nsec = 10 * resources->my_rank * 1000000, /* Convert ms to ns */
	};
	nanosleep(&sleep_ts, NULL);

	/* Create doca_dpa context */
	result = doca_dpa_create(resources->pf_doca_device, &(resources->pf_doca_dpa));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA DPA context: %s", doca_error_get_descr(result));
		goto close_doca_dev;
	}

	/* Set doca_dpa app */
	result = doca_dpa_set_app(resources->pf_doca_dpa, dpa_all2all_app);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DOCA DPA app: %s", doca_error_get_descr(result));
		goto destroy_doca_dpa;
	}

	/* Start doca_dpa context */
	result = doca_dpa_start(resources->pf_doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA DPA context: %s", doca_error_get_descr(result));
		goto destroy_doca_dpa;
	}

#ifdef DOCA_ARCH_DPU
	if (resources->rdma_doca_device != resources->pf_doca_device) {
		result = doca_dpa_device_extend(resources->pf_doca_dpa,
						resources->rdma_doca_device,
						&resources->rdma_doca_dpa);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to extend DOCA DPA context: %s", doca_error_get_descr(result));
			goto destroy_doca_dpa;
		}

		result = doca_dpa_get_dpa_handle(resources->rdma_doca_dpa, &resources->rdma_doca_dpa_handle);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get DOCA DPA context handle: %s", doca_error_get_descr(result));
			goto destroy_rdma_doca_dpa;
		}
	} else {
		resources->rdma_doca_dpa = resources->pf_doca_dpa;
	}
#else
	resources->rdma_doca_dpa = resources->pf_doca_dpa;
#endif

	return result;

#ifdef DOCA_ARCH_DPU
destroy_rdma_doca_dpa:
	tmp_result = doca_dpa_destroy(resources->rdma_doca_dpa);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA DPA context: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
#endif
destroy_doca_dpa:
	tmp_result = doca_dpa_destroy(resources->pf_doca_dpa);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA DPA context: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
close_doca_dev:
	tmp_result = doca_dev_close(resources->pf_doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA DPA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
#ifdef DOCA_ARCH_DPU
	tmp_result = doca_dev_close(resources->rdma_doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA DPA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
#endif

	return result;
}

/*
 * Create DOCA mmap
 *
 * @doca_device [in]: device to associate to mmap context
 * @mmap_permissions [in]: capabilities enabled on the mmap
 * @memrange_addr [in]: memrange address to set on the mmap
 * @memrange_len [in]: length of memrange to set on the mmap
 * @mmap [out]: Created mmap
 * @dpa_mmap_handle [out]: Created DPA mmap handle
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_mmap(struct doca_dev *doca_device,
				unsigned int mmap_permissions,
				void *memrange_addr,
				size_t memrange_len,
				struct doca_mmap **mmap,
				doca_dpa_dev_mmap_t *dpa_mmap_handle)
{
	doca_error_t result;
	doca_error_t tmp_result;

	/* Creating DOCA mmap */
	result = doca_mmap_create(mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA mmap: %s", doca_error_get_descr(result));
		return result;
	}

	/* Add DOCA device to DOCA mmap */
	result = doca_mmap_add_dev(*mmap, doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to add DOCA device: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/* Set permissions for DOCA mmap */
	result = doca_mmap_set_permissions(*mmap, mmap_permissions);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions for DOCA mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/* Set memrange for DOCA mmap */
	result = doca_mmap_set_memrange(*mmap, memrange_addr, memrange_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set memrange for DOCA mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/* Start DOCA mmap */
	result = doca_mmap_start(*mmap);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA mmap: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	/* Get DOCA mmap DPA handle */
	result = doca_mmap_dev_get_dpa_handle(*mmap, doca_device, dpa_mmap_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get DOCA mmap DPA handle: %s", doca_error_get_descr(result));
		goto destroy_mmap;
	}

	return result;

destroy_mmap:
	/* destroy DOCA mmap */
	tmp_result = doca_mmap_destroy(*mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Create DOCA sync event from remote net export and its DPA handle
 *
 * @doca_dpa [in]: DOCA DPA context
 * @doca_device [in]: DOCA device
 * @remote_event_export_data [in]: export data of the remote net DOCA sync event to create
 * @remote_event_export_size [in]: export size of the remote net DOCA sync event to create
 * @remote_event [out]: Created remote net DOCA sync event
 * @remote_event_dpa_handle [out]: DPA handle for the created remote net DOCA sync event
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_doca_dpa_sync_event_from_export(struct doca_dpa *doca_dpa,
							   struct doca_dev *doca_device,
							   const uint8_t *remote_event_export_data,
							   size_t remote_event_export_size,
							   struct doca_sync_event_remote_net **remote_event,
							   doca_dpa_dev_sync_event_remote_net_t *remote_event_dpa_handle)
{
	doca_error_t result, tmp_result;

	result = doca_sync_event_remote_net_create_from_export(doca_device,
							       remote_event_export_data,
							       remote_event_export_size,
							       remote_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create remote net DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_remote_net_get_dpa_handle(*remote_event, doca_dpa, remote_event_dpa_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to export remote net DOCA sync event to DPA: %s", doca_error_get_descr(result));
		goto destroy_remote_event;
	}

	return result;

destroy_remote_event:
	tmp_result = doca_sync_event_remote_net_destroy(*remote_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy remote net DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Prepare the memory needed for the DOCA DPA all to all, including the sendbuf and recvbufs memory handlers and remote
 * keys, and getting the remote recvbufs addresses from the remote processes.
 *
 * @resources [in/out]: All to all resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_dpa_a2a_memory(struct a2a_resources *resources)
{
	/* DOCA mmap recvbuf rdma export */
	const void *recv_mmap_export;
	/* DOCA mmap recvbuf rdma export length */
	size_t recv_mmap_export_len;
	/* DOCA mmap exports of remote processes */
	void **recvbufs_mmap_exports = NULL;
	/* DOCA mmap exports lengths of remote processes */
	size_t *recvbufs_mmap_exports_lens = NULL;
	/* Host memory for all remote processes recvbufs */
	uintptr_t *recvbufs = NULL;
	/*
	 * Define DOCA DPA host memory access flags
	 * mem_access_read gives read access to the sendbuf
	 * mem_access_write gives write access to the recvbuf
	 */
	const unsigned int mem_access_read = DOCA_ACCESS_FLAG_LOCAL_READ_ONLY | DOCA_ACCESS_FLAG_RDMA_READ;
	const unsigned int mem_access_write = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_WRITE;
	/* Size of the buffers (send and receive) */
	size_t buf_size;
	MPI_Aint lb, extent;
	MPI_Request reqs[5];
	doca_error_t result, tmp_result;
	int i, j;
	doca_dpa_dev_mmap_t recvbuf_dpa_mmap_handle;

	/* Get the extent of the datatype and calculate the size of the buffers */
	MPI_Type_get_extent(resources->msg_type, &lb, &extent);
	buf_size = extent * resources->mesg_count * resources->num_ranks;
	resources->extent = extent;

	/* create mmap for process send buff */
	result = create_mmap(resources->rdma_doca_device,
			     mem_access_read,
			     resources->sendbuf,
			     buf_size,
			     &(resources->sendbuf_mmap),
			     &(resources->sendbuf_dpa_mmap_handle));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap for sendbuf: %s", doca_error_get_descr(result));
		return result;
	}

	/* create mmap for process receive buff */
	result = create_mmap(resources->rdma_doca_device,
			     mem_access_write,
			     resources->recvbuf,
			     buf_size,
			     &(resources->recvbuf_mmap),
			     &recvbuf_dpa_mmap_handle);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create mmap for recvbuf: %s", doca_error_get_descr(result));
		goto destroy_sendbuf_mmap;
	}

	/* create mmap export to the receive buffer for rdma operation */
	result = doca_mmap_export_rdma(resources->recvbuf_mmap,
				       resources->rdma_doca_device,
				       &recv_mmap_export,
				       &recv_mmap_export_len);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to export mmap for recvbuf: %s", doca_error_get_descr(result));
		goto destroy_recvbuf_mmap;
	}

	/* Allocate memory to hold recvbufs mmap exports lengths of all the processes */
	recvbufs_mmap_exports_lens = (size_t *)calloc(resources->num_ranks, sizeof(*recvbufs_mmap_exports_lens));
	if (recvbufs_mmap_exports_lens == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for recv mmap export lengths");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_recvbuf_mmap;
	}

	/* Allocate memory to hold recvbufs mmap exports of all the processes */
	recvbufs_mmap_exports = (void **)calloc(resources->num_ranks, recv_mmap_export_len);
	if (recvbufs_mmap_exports == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for recv mmap exports");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_mmap_exports_lens;
	}

	/* Send the local recvbuf export length and receive all the remote recvbuf exports lengths using Allgather */
	MPI_Iallgather(&recv_mmap_export_len,
		       sizeof(recv_mmap_export_len),
		       MPI_BYTE,
		       recvbufs_mmap_exports_lens,
		       sizeof(recv_mmap_export_len),
		       MPI_BYTE,
		       resources->comm,
		       &reqs[0]);

	result = mpi_request_wait_timeout(&reqs[0], MAX_MPI_WAIT_TIME);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting on allgather: %s", doca_error_get_descr(result));
		goto free_mmap_exports;
	}

	/* Send the local recvbuf mmap export and receive all the remote recvbuf mmap exports using Allgather */
	MPI_Iallgather(recv_mmap_export,
		       recv_mmap_export_len,
		       MPI_BYTE,
		       recvbufs_mmap_exports,
		       recv_mmap_export_len,
		       MPI_BYTE,
		       resources->comm,
		       &reqs[1]);

	result = mpi_request_wait_timeout(&reqs[1], MAX_MPI_WAIT_TIME);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting on allgather: %s", doca_error_get_descr(result));
		goto free_mmap_exports;
	}

	resources->export_mmaps = calloc(resources->num_ranks, sizeof(*(resources->export_mmaps)));
	if (resources->export_mmaps == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for export mmaps");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_mmap_exports;
	}

	resources->export_mmaps_dpa_handle =
		calloc(resources->num_ranks, sizeof(*(resources->export_mmaps_dpa_handle)));
	if (resources->export_mmaps_dpa_handle == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for export mmaps dpa handle");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_export_mmaps;
	}

	for (i = 0; i < resources->num_ranks; i++) {
		/* skip to export index */
		j = i * recv_mmap_export_len;
		/* create mmap for process send buff */
		result = doca_mmap_create_from_export(NULL,
						      (const void *)&(((char *)recvbufs_mmap_exports)[j]),
						      recvbufs_mmap_exports_lens[i],
						      resources->rdma_doca_device,
						      &(resources->export_mmaps[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create mmap from export: %s", doca_error_get_descr(result));
			goto destroy_export_mmaps;
		}

		/* Get DOCA mmap DPA handle */
		result = doca_mmap_dev_get_dpa_handle(resources->export_mmaps[i],
						      resources->rdma_doca_device,
						      &(resources->export_mmaps_dpa_handle[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get DOCA mmap DPA handle: %s", doca_error_get_descr(result));
			goto destroy_export_mmaps;
		}
	}

	/* Allocate memory to hold recvbufs of all the processes */
	recvbufs = (uintptr_t *)calloc(resources->num_ranks, sizeof(*(recvbufs)));
	if (recvbufs == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for recvbufs of all the processes");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_export_mmaps;
	}

	/* Send the local recvbuf export length and receive all the remote recvbuf exports lengths using Allgather */
	MPI_Iallgather(&(resources->recvbuf),
		       sizeof(uintptr_t),
		       MPI_BYTE,
		       (void *)recvbufs,
		       sizeof(uintptr_t),
		       MPI_BYTE,
		       resources->comm,
		       &reqs[2]);
	result = mpi_request_wait_timeout(&reqs[2], MAX_MPI_WAIT_TIME);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting on allgather: %s", doca_error_get_descr(result));
		goto free_recvbufs;
	}

	/* Allocate DPA memory to hold the recvbufs addresses */
	result = doca_dpa_mem_alloc(resources->rdma_doca_dpa,
				    (resources->num_ranks * sizeof(uintptr_t)),
				    &(resources->devptr_recvbufs));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DOCA DPA memory: %s", doca_error_get_descr(result));
		goto free_recvbufs;
	}

	/* Copy the recvbufs addresses array from the host memory to the device memory */
	result = doca_dpa_h2d_memcpy(resources->rdma_doca_dpa,
				     resources->devptr_recvbufs,
				     (void *)recvbufs,
				     resources->num_ranks * sizeof(uintptr_t));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to copy DOCA DPA memory from host to device: %s", doca_error_get_descr(result));
		goto free_devptr_recvbufs;
	}

	/* Allocate DPA memory to hold the recvbufs mmap handles */
	result = doca_dpa_mem_alloc(resources->rdma_doca_dpa,
				    (resources->num_ranks * sizeof(doca_dpa_dev_mmap_t)),
				    &(resources->devptr_recvbufs_mmap_handles));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DOCA DPA memory: %s", doca_error_get_descr(result));
		goto free_devptr_recvbufs;
	}

	/* Copy the recvbufs mmap handles array from the host memory to the device memory */
	result = doca_dpa_h2d_memcpy(resources->rdma_doca_dpa,
				     resources->devptr_recvbufs_mmap_handles,
				     (void *)resources->export_mmaps_dpa_handle,
				     resources->num_ranks * sizeof(doca_dpa_dev_mmap_t));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to copy DOCA DPA memory from host to device: %s", doca_error_get_descr(result));
		goto free_devptr_recvbufs_mmap_handles;
	}

	resources->rp_remote_kernel_events_export_sizes =
		calloc(resources->num_ranks, sizeof(*(resources->rp_remote_kernel_events_export_sizes)));
	if (resources->rp_remote_kernel_events_export_sizes == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for rp_remote_kernel_events_export_sizes");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_devptr_recvbufs;
	}

	/* Send the local process' remote kernel event and receive all the remote kernel events using Alltoall */
	MPI_Ialltoall(resources->lp_remote_kernel_events_export_sizes,
		      sizeof(*(resources->lp_remote_kernel_events_export_sizes)),
		      MPI_BYTE,
		      resources->rp_remote_kernel_events_export_sizes,
		      sizeof(*(resources->rp_remote_kernel_events_export_sizes)),
		      MPI_BYTE,
		      resources->comm,
		      &reqs[3]);

	result = mpi_request_wait_timeout(&reqs[3], MAX_MPI_WAIT_TIME);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting on allgather: %s", doca_error_get_descr(result));
		goto free_remote_kernel_events_exports;
	}

	resources->rp_remote_kernel_events_export_data =
		calloc(resources->num_ranks, resources->rp_remote_kernel_events_export_sizes[0]);
	if (resources->rp_remote_kernel_events_export_data == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for rp_remote_kernel_events_export_data");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_remote_kernel_events_exports;
	}

	/* Send the local process' remote kernel event and receive all the remote kernel events using Alltoall */
	MPI_Ialltoall(resources->lp_remote_kernel_events_export_data,
		      resources->lp_remote_kernel_events_export_sizes[0],
		      MPI_BYTE,
		      resources->rp_remote_kernel_events_export_data,
		      resources->rp_remote_kernel_events_export_sizes[0],
		      MPI_BYTE,
		      resources->comm,
		      &reqs[4]);

	result = mpi_request_wait_timeout(&reqs[4], MAX_MPI_WAIT_TIME);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting on allgather: %s", doca_error_get_descr(result));
		goto free_remote_kernel_events_exports;
	}

	resources->rp_kernel_events = calloc(resources->num_ranks, sizeof(*(resources->rp_kernel_events)));
	if (resources->rp_kernel_events == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for rp_kernel_events");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_remote_kernel_events_exports;
	}

	resources->rp_kernel_events_dpa_handles =
		calloc(resources->num_ranks, sizeof(*(resources->rp_kernel_events_dpa_handles)));
	if (resources->rp_kernel_events_dpa_handles == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for rp_kernel_events_dpa_handles");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_remote_kernel_events;
	}

	for (i = 0; i < resources->num_ranks; i++) {
		/* skip to export index */
		j = i * resources->rp_remote_kernel_events_export_sizes[i];
		result = create_doca_dpa_sync_event_from_export(
			resources->rdma_doca_dpa,
			resources->rdma_doca_device,
			(const uint8_t *)&(((char *)resources->rp_remote_kernel_events_export_data)[j]),
			resources->rp_remote_kernel_events_export_sizes[i],
			&(resources->rp_kernel_events[i]),
			&(resources->rp_kernel_events_dpa_handles[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create DOCA sync event from export: %s", doca_error_get_descr(result));
			goto free_remote_kernel_events_dpa_handles;
		}
	}

	/* Allocate DPA memory to hold the remote kernel events */
	result = doca_dpa_mem_alloc(resources->rdma_doca_dpa,
				    resources->num_ranks * sizeof(*(resources->rp_kernel_events_dpa_handles)),
				    &(resources->devptr_rp_remote_kernel_events));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DOCA DPA memory: %s", doca_error_get_descr(result));
		goto destroy_kernel_events_from_export;
	}

	/* Copy the remote kernel events from the host memory to the device memory */
	result = doca_dpa_h2d_memcpy(resources->rdma_doca_dpa,
				     resources->devptr_rp_remote_kernel_events,
				     (void *)resources->rp_kernel_events_dpa_handles,
				     resources->num_ranks * sizeof(*(resources->rp_kernel_events_dpa_handles)));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to copy DOCA DPA memory from host to device: %s", doca_error_get_descr(result));
		goto destroy_kernel_events_from_export;
	}

	/* Allocate DPA memory to hold the local remote kernel events */
	result = doca_dpa_mem_alloc(resources->rdma_doca_dpa,
				    resources->num_ranks * sizeof(*(resources->kernel_events_handle)),
				    &(resources->devptr_kernel_events_handle));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DOCA DPA memory: %s", doca_error_get_descr(result));
		goto free_rp_remote_kernel_events_dpa;
	}

	/* Copy the remote kernel events from the host memory to the device memory */
	result = doca_dpa_h2d_memcpy(resources->rdma_doca_dpa,
				     resources->devptr_kernel_events_handle,
				     (void *)resources->kernel_events_handle,
				     resources->num_ranks * sizeof(*(resources->kernel_events_handle)));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to copy DOCA DPA memory from host to device: %s", doca_error_get_descr(result));
		goto free_kernel_events_handle_dpa;
	}

	free(recvbufs);

	/* free recv mmaps exports pointers since no longer needed after creating mmaps from exports */
	free(recvbufs_mmap_exports);
	free(recvbufs_mmap_exports_lens);

	/* Free the local process' remote kernel event exports since we don't need them anymore */
	free(resources->lp_remote_kernel_events_export_sizes);
	free(resources->lp_remote_kernel_events_export_data);

	/* free remote processes kernel event exports since we don't need them after creating the DPA handle */
	free(resources->rp_remote_kernel_events_export_data);
	free(resources->rp_remote_kernel_events_export_sizes);

	return result;

free_kernel_events_handle_dpa:
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_kernel_events_handle);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
free_rp_remote_kernel_events_dpa:
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_rp_remote_kernel_events);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_kernel_events_from_export:
	for (i = 0; i < resources->num_ranks; i++) {
		if (resources->rp_kernel_events[i] != NULL) {
			tmp_result = doca_sync_event_remote_net_destroy(resources->rp_kernel_events[i]);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy remote net DOCA sync event: %s",
					     doca_error_get_descr(tmp_result));
				DOCA_ERROR_PROPAGATE(result, tmp_result);
			}
		}
	}
free_remote_kernel_events_dpa_handles:
	free(resources->rp_kernel_events_dpa_handles);
free_remote_kernel_events:
	free(resources->rp_kernel_events);
free_remote_kernel_events_exports:
	free(resources->rp_remote_kernel_events_export_data);
	free(resources->rp_remote_kernel_events_export_sizes);
free_devptr_recvbufs_mmap_handles:
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_recvbufs_mmap_handles);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
free_devptr_recvbufs:
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_recvbufs);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
free_recvbufs:
	free(recvbufs);
destroy_export_mmaps:
	for (j = 0; j < i; j++) {
		if (resources->export_mmaps[j] != NULL) {
			tmp_result = doca_mmap_destroy(resources->export_mmaps[j]);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
				DOCA_ERROR_PROPAGATE(result, tmp_result);
			}
		}
	}
free_export_mmaps:
	free(resources->export_mmaps);
free_mmap_exports:
	free(recvbufs_mmap_exports);
free_mmap_exports_lens:
	free(recvbufs_mmap_exports_lens);
destroy_recvbuf_mmap:
	tmp_result = doca_mmap_destroy(resources->recvbuf_mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_sendbuf_mmap:
	tmp_result = doca_mmap_destroy(resources->sendbuf_mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Connect the local process' DOCA RDMA contexts to the remote processes' DOCA DPA RDMAs.
 * rdma number i in each process would be connected to an rdma in process rank i.
 *
 * @resources [in]: All to all resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t connect_dpa_a2a_rdmas(struct a2a_resources *resources)
{
	/* Local rdma connection details */
	const void *local_connection_details = NULL;
	/* Remote rdma connection details */
	const void *remote_connection_details = NULL;
	/* Length of addresses */
	size_t local_connection_details_len, remote_connection_details_len;
	/* rdma connection object */
	struct doca_rdma_connection *connection = NULL;
	/* Tags for the MPI send and recv for address and address length */
	const int addr_tag = 1;
	const int addr_len_tag = 2;
	/* MPI request used for synchronization between processes */
	MPI_Request reqs[4];
	int i;
	doca_error_t result = DOCA_SUCCESS;

	for (i = 0; i < resources->num_ranks; i++) {
		/*
		 * Get the local rdma connection details with the index
		 * same as the rank of the process we are going to send to
		 */
		result = doca_rdma_export(resources->rdmas[i],
					  &local_connection_details,
					  &local_connection_details_len,
					  &connection);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get DOCA rdma connection details: %s", doca_error_get_descr(result));
			return result;
		}

		/* Send and receive the addresses using MPI Isend and Recv */
		MPI_Isend(&local_connection_details_len, 1, MPI_INT64_T, i, addr_len_tag, resources->comm, &reqs[0]);
		MPI_Isend(local_connection_details,
			  local_connection_details_len,
			  MPI_CHAR,
			  i,
			  addr_tag,
			  resources->comm,
			  &reqs[1]);

		MPI_Irecv(&remote_connection_details_len, 1, MPI_INT64_T, i, addr_len_tag, resources->comm, &reqs[2]);

		result = mpi_request_wait_timeout(&reqs[2], MAX_MPI_WAIT_TIME);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Timed out waiting on receiving remote connection details length: %s",
				     doca_error_get_descr(result));
			return result;
		}

		remote_connection_details = malloc(remote_connection_details_len);
		if (remote_connection_details == NULL) {
			DOCA_LOG_ERR("Failed to allocate memory for remote rdma connection details");
			return DOCA_ERROR_NO_MEMORY;
		}
		MPI_Irecv((void *)remote_connection_details,
			  remote_connection_details_len,
			  MPI_CHAR,
			  i,
			  addr_tag,
			  resources->comm,
			  &reqs[3]);

		result = mpi_request_wait_timeout(&reqs[3], MAX_MPI_WAIT_TIME);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Timed out waiting on receiving remote connection details: %s",
				     doca_error_get_descr(result));
			free((void *)remote_connection_details);
			return result;
		}

		/*
		 * Connect to the rdma of the remote process.
		 * The local rdma of index i will be connected to an rdma of a remote process of rank i.
		 */
		result = doca_rdma_connect(resources->rdmas[i],
					   remote_connection_details,
					   remote_connection_details_len,
					   connection);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to connect DOCA rdma: %s", doca_error_get_descr(result));
			free((void *)remote_connection_details);
			return result;
		}

		result = mpi_request_wait_timeout(&reqs[0], MAX_MPI_WAIT_TIME);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Timed out waiting on sending local connection details length: %s",
				     doca_error_get_descr(result));
			free((void *)remote_connection_details);
			return result;
		}

		result = mpi_request_wait_timeout(&reqs[1], MAX_MPI_WAIT_TIME);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Timed out waiting on sending local connection details: %s",
				     doca_error_get_descr(result));
			free((void *)remote_connection_details);
			return result;
		}

		free((void *)remote_connection_details);
	}

	return result;
}

/*
 * Create DOCA rdma instance
 *
 * @doca_dpa [in]: DPA context to set datapath on
 * @doca_device [in]: device to associate to rdma context
 * @rdma_caps [in]: capabilities enabled on the rdma context
 * @dpa_completion [in]: DPA completion context to be attached for the rdma context
 * @rdma [out]: Created rdma
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_rdma(struct doca_dpa *doca_dpa,
				struct doca_dev *doca_device,
				unsigned int rdma_caps,
				struct doca_dpa_completion *dpa_completion,
				struct doca_rdma **rdma)
{
	struct doca_ctx *rdma_as_doca_ctx;
	doca_error_t result, tmp_result;

	/* Creating DOCA rdma instance */
	result = doca_rdma_create(doca_device, rdma);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA rdma instance: %s", doca_error_get_descr(result));
		return result;
	}

	/* Setup DOCA rdma as DOCA context */
	rdma_as_doca_ctx = doca_rdma_as_ctx(*rdma);

	/* Set permissions for DOCA rdma */
	result = doca_rdma_set_permissions(*rdma, rdma_caps);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set permissions for DOCA rdma: %s", doca_error_get_descr(result));
		goto destroy_rdma;
	}

	/* Set grh flag for DOCA rdma */
	result = doca_rdma_set_grh_enabled(*rdma, true);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set grh for DOCA rdma: %s", doca_error_get_descr(result));
		goto destroy_rdma;
	}

	/* Set datapath of DOCA rdma context on DPA */
	result = doca_ctx_set_datapath_on_dpa(rdma_as_doca_ctx, doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set datapath for DOCA rdma on DPA: %s", doca_error_get_descr(result));
		goto destroy_rdma;
	}

	/* Attach DPA completion context for DOCA rdma context */
	result = doca_rdma_dpa_completion_attach(*rdma, dpa_completion);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to attach DPA completion context for DOCA rdma: %s", doca_error_get_descr(result));
		goto destroy_rdma;
	}

	/* Start DOCA rdma context */
	result = doca_ctx_start(rdma_as_doca_ctx);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start context for DOCA rdma: %s", doca_error_get_descr(result));
		goto destroy_rdma;
	}

	return result;

destroy_rdma:
	tmp_result = doca_rdma_destroy(*rdma);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA rdma instance: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Destroy DOCA rdma instance
 *
 * @rdma [in]: rdma context to destroy
 * @doca_device [in]: device associated to rdma context
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t destroy_rdma(struct doca_rdma *rdma, struct doca_dev *doca_device)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result = DOCA_SUCCESS;

	tmp_result = doca_ctx_stop(doca_rdma_as_ctx(rdma));
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to stop DOCA rdma context: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	tmp_result = doca_rdma_destroy(rdma);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA rdma instance: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Prepare the DOCA DPA completion contexts
 *
 * @resources [in/out]: All to all resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_dpa_a2a_dpa_completions(struct a2a_resources *resources)
{
	int i, j;
	doca_error_t result = DOCA_SUCCESS, tmp_result;

	/* Create dpa completion contexts as number of the processes */
	resources->dpa_completions = calloc(resources->num_ranks, sizeof(*(resources->dpa_completions)));
	if (resources->dpa_completions == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for DOCA DPA completions");
		return DOCA_ERROR_NO_MEMORY;
	}
	for (i = 0; i < resources->num_ranks; i++) {
		result = doca_dpa_completion_create(resources->rdma_doca_dpa,
						    resources->num_ranks,
						    &(resources->dpa_completions[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create DOCA DPA completion: %s", doca_error_get_descr(result));
			goto destroy_dpa_completions;
		}

		result = doca_dpa_completion_start(resources->dpa_completions[i]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to start DOCA DPA completion: %s", doca_error_get_descr(result));
			tmp_result = doca_dpa_completion_destroy(resources->dpa_completions[i]);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy DOCA DPA completion instance: %s",
					     doca_error_get_descr(tmp_result));
				DOCA_ERROR_PROPAGATE(result, tmp_result);
			}
			goto destroy_dpa_completions;
		}
	}

	return result;

destroy_dpa_completions:
	for (j = 0; j < i; j++) {
		tmp_result = doca_dpa_completion_destroy(resources->dpa_completions[j]);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA DPA completion instance: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->dpa_completions);

	return result;
}

/*
 * Prepare the DOCA rdma, which includes creating the RDMA contexts and their handlers, connecting them to
 * the remote processes' RDMA contexts and allocating DOCA DPA device memory to hold the handlers so that
 * they can be used in a DOCA DPA kernel function.
 *
 * @resources [in/out]: All to all resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t prepare_dpa_a2a_rdmas(struct a2a_resources *resources)
{
	/* Access flags for the rdma */
	const unsigned int rdma_access = DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
					 DOCA_ACCESS_FLAG_RDMA_WRITE | DOCA_ACCESS_FLAG_RDMA_ATOMIC;
	/* DOCA DPA rdma handlers */
	doca_dpa_dev_rdma_t *rdma_handlers;
	int i, j;
	doca_error_t result, tmp_result;

	/* Create rdmas as number of the processes */
	resources->rdmas = calloc(resources->num_ranks, sizeof(*(resources->rdmas)));
	if (resources->rdmas == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for DOCA DPA RDMAs");
		return DOCA_ERROR_NO_MEMORY;
	}
	for (i = 0; i < resources->num_ranks; i++) {
		result = create_rdma(resources->rdma_doca_dpa,
				     resources->rdma_doca_device,
				     rdma_access,
				     resources->dpa_completions[i],
				     &(resources->rdmas[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create DOCA rdma: %s", doca_error_get_descr(result));
			goto destroy_rdmas;
		}
	}

	/* Connect local RDMA contexts to the remote RDMA contexts */
	result = connect_dpa_a2a_rdmas(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to connect DOCA rdma: %s", doca_error_get_descr(result));
		goto destroy_rdmas;
	}

	/* Create device handlers for the RDMA contexts */
	rdma_handlers = (doca_dpa_dev_rdma_t *)calloc(resources->num_ranks, sizeof(*rdma_handlers));
	if (rdma_handlers == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for DOCA DPA device rdma handlers");
		goto destroy_rdmas;
	}
	for (j = 0; j < resources->num_ranks; j++) {
		result = doca_rdma_get_dpa_handle(resources->rdmas[j], &(rdma_handlers[j]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get DOCA rdma DPA handler: %s", doca_error_get_descr(result));
			goto free_rdma_handlers;
		}
	}

	/* Allocate DPA memory to hold the RDMA handlers */
	result = doca_dpa_mem_alloc(resources->rdma_doca_dpa,
				    sizeof(*rdma_handlers) * resources->num_ranks,
				    &(resources->devptr_rdmas));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DOCA DPA memory: %s", doca_error_get_descr(result));
		goto free_rdma_handlers;
	}

	/* Copy the rdma handlers from the host memory to the device memory */
	result = doca_dpa_h2d_memcpy(resources->rdma_doca_dpa,
				     resources->devptr_rdmas,
				     (void *)rdma_handlers,
				     sizeof(*rdma_handlers) * resources->num_ranks);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to copy DOCA DPA memory from host to device: %s", doca_error_get_descr(result));
		goto free_rdma_handlers_dpa;
	}

	/* Free the rdma handlers */
	free(rdma_handlers);

	return result;

free_rdma_handlers_dpa:
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_rdmas);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
free_rdma_handlers:
	free(rdma_handlers);
destroy_rdmas:
	for (j = 0; j < i; j++) {
		tmp_result = destroy_rdma(resources->rdmas[j], resources->rdma_doca_device);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA rdma instance: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->rdmas);

	return result;
}

/*
 * Create DOCA sync event to be published by the DPA and subscribed by the CPU
 *
 * @doca_dpa [in]: DOCA DPA context
 * @doca_device [in]: DOCA device
 * @comp_event [out]: Created DOCA sync event that is published by the DPA and subscribed by the CPU
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_doca_dpa_completion_sync_event(struct doca_dpa *doca_dpa,
							  struct doca_dev *doca_device,
							  struct doca_sync_event **comp_event)
{
	doca_error_t result, tmp_result;

	result = doca_sync_event_create(comp_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_add_publisher_location_dpa(*comp_event, doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA as publisher for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	result = doca_sync_event_add_subscriber_location_cpu(*comp_event, doca_device);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set CPU as subscriber for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	result = doca_sync_event_start(*comp_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	return result;

destroy_comp_event:
	tmp_result = doca_sync_event_destroy(*comp_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Create DOCA sync event to be published by remote net and subscribed by the DPA
 *
 * @doca_dpa [in]: DOCA DPA context
 * @kernel_event [out]: Created DOCA sync event that is published by remote net and subscribed by the DPA
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_doca_dpa_remote_net_sync_event(struct doca_dpa *doca_dpa,
							  struct doca_sync_event **kernel_event)
{
	doca_error_t result, tmp_result;

	result = doca_sync_event_create(kernel_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event: %s", doca_error_get_descr(result));
		return result;
	}

	result = doca_sync_event_add_publisher_location_remote_net(*kernel_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set remote as publisher for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_kernel_event;
	}

	result = doca_sync_event_add_subscriber_location_dpa(*kernel_event, doca_dpa);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to set DPA as subscriber for DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_kernel_event;
	}

	result = doca_sync_event_start(*kernel_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to start DOCA sync event: %s", doca_error_get_descr(result));
		goto destroy_kernel_event;
	}

	return result;

destroy_kernel_event:
	tmp_result = doca_sync_event_destroy(*kernel_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA sync event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}

/*
 * Create the needed DOCA sync events for the All to All:
 *	One kernel completion event, the publisher is the DPA and the subscriber is the host.
 *	Number of ranks kernel events, the publisher and subscriber is the DPA.
 *
 * @resources [in/out]: All to all resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t create_dpa_a2a_events(struct a2a_resources *resources)
{
	int i, j;
	doca_error_t result, tmp_result;
	const uint8_t **lp_remote_kernel_events_export_data_arr = NULL;

	/* Create DOCA DPA kernel completion event*/
	result = create_doca_dpa_completion_sync_event(resources->pf_doca_dpa,
						       resources->pf_doca_device,
						       &(resources->comp_event));
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create host completion event: %s", doca_error_get_descr(result));
		return result;
	}

	/* Create DOCA DPA events to be used inside of the kernel */
	resources->kernel_events = calloc(resources->num_ranks, sizeof(*(resources->kernel_events)));
	if (resources->kernel_events == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for kernel events");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_comp_event;
	}
	for (i = 0; i < resources->num_ranks; i++) {
		result =
			create_doca_dpa_remote_net_sync_event(resources->rdma_doca_dpa, &(resources->kernel_events[i]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create kernel event: %s", doca_error_get_descr(result));
			goto destroy_kernel_events;
		}
	}

	/* Create DOCA DPA events handles */
	resources->kernel_events_handle = calloc(resources->num_ranks, sizeof(*(resources->kernel_events_handle)));
	if (resources->kernel_events_handle == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for kernel events handles");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_kernel_events_handles;
	}

	for (j = 0; j < resources->num_ranks; j++) {
		/* Export the kernel events */
		result = doca_sync_event_get_dpa_handle(resources->kernel_events[j],
							resources->rdma_doca_dpa,
							&(resources->kernel_events_handle[j]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to export kernel event: %s", doca_error_get_descr(result));
			goto destroy_kernel_events_handles;
		}
	}

	/* Remote export the kernel events */
	resources->lp_remote_kernel_events_export_sizes =
		calloc(resources->num_ranks, sizeof(*(resources->lp_remote_kernel_events_export_sizes)));
	if (resources->lp_remote_kernel_events_export_sizes == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for kernel events export sizes");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_kernel_events_handles;
	}

	lp_remote_kernel_events_export_data_arr = calloc(resources->num_ranks, sizeof(const uint8_t *));

	for (j = 0; j < resources->num_ranks; j++) {
		/* Export the kernel events */
		result = doca_sync_event_export_to_remote_net(resources->kernel_events[j],
							      &(lp_remote_kernel_events_export_data_arr[j]),
							      &(resources->lp_remote_kernel_events_export_sizes[j]));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to remote export kernel event: %s", doca_error_get_descr(result));
			goto free_remote_kernel_events_export_data_arr;
		}
	}

	resources->lp_remote_kernel_events_export_data =
		calloc(resources->num_ranks, resources->lp_remote_kernel_events_export_sizes[0]);
	if (resources->lp_remote_kernel_events_export_data == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for kernel events export data");
		result = DOCA_ERROR_NO_MEMORY;
		goto free_remote_kernel_events_export_data_arr;
	}

	for (j = 0; j < resources->num_ranks; j++) {
		/* skip to export index */
		i = j * resources->lp_remote_kernel_events_export_sizes[j];
		memcpy(&(resources->lp_remote_kernel_events_export_data[i]),
		       lp_remote_kernel_events_export_data_arr[j],
		       resources->lp_remote_kernel_events_export_sizes[j]);
	}

	free(lp_remote_kernel_events_export_data_arr);

	return result;

free_remote_kernel_events_export_data_arr:
	free(lp_remote_kernel_events_export_data_arr);

	free(resources->lp_remote_kernel_events_export_sizes);
destroy_kernel_events_handles:
	free(resources->kernel_events_handle);
destroy_kernel_events:
	for (j = 0; j < i; j++) {
		tmp_result = doca_sync_event_destroy(resources->kernel_events[j]);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy kernel_event: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->kernel_events);
destroy_comp_event:
	tmp_result = doca_sync_event_destroy(resources->comp_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy comp_event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t dpa_a2a_init(struct a2a_resources *resources)
{
	doca_error_t result, tmp_result;
	int i;

	/* divide the two devices (can be the same) on all processes equally */
	if (resources->my_rank >= ((double)resources->num_ranks / 2.0)) {
		strcpy(resources->pf_device_name, pf_device2_name);
		strcpy(resources->rdma_device_name, rdma_device2_name);
	} else {
		strcpy(resources->pf_device_name, pf_device1_name);
		strcpy(resources->rdma_device_name, rdma_device1_name);
	}

	/* Create DOCA DPA context*/
	result = create_dpa_context(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA DPA device: %s", doca_error_get_descr(result));
		return result;
	}

	result = create_dpa_a2a_events(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA DPA events: %s", doca_error_get_descr(result));
		goto destroy_dpa;
	}

	/* Prepare DOCA DPA completion contexts all to all resources */
	result = prepare_dpa_a2a_dpa_completions(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare DOCA DPA completion contexts resources: %s",
			     doca_error_get_descr(result));
		goto destroy_events;
	}

	/* Prepare DOCA RDMA contexts all to all resources */
	result = prepare_dpa_a2a_rdmas(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare DOCA RDMA contexts resources: %s", doca_error_get_descr(result));
		goto destroy_dpa_completions;
	}

	/* Prepare DOCA DPA all to all memory */
	result = prepare_dpa_a2a_memory(resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to prepare DOCA DPA memory resources: %s", doca_error_get_descr(result));
		goto destroy_rdmas;
	}

	return result;

destroy_rdmas:
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_rdmas);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	/* Destroy DOCA RDMA contexts */
	for (i = 0; i < resources->num_ranks; i++) {
		tmp_result = destroy_rdma(resources->rdmas[i], resources->rdma_doca_device);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA rdma instance: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->rdmas);
destroy_dpa_completions:
	/* Destroy DOCA DPA completion contexts */
	for (i = 0; i < resources->num_ranks; i++) {
		tmp_result = doca_dpa_completion_destroy(resources->dpa_completions[i]);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA DPA completion instance: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->dpa_completions);
destroy_events:
	free(resources->lp_remote_kernel_events_export_data);
	free(resources->lp_remote_kernel_events_export_sizes);
	free(resources->kernel_events_handle);
	for (i = 0; i < resources->num_ranks; i++) {
		tmp_result = doca_sync_event_destroy(resources->kernel_events[i]);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy kernel_event: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->kernel_events);
	tmp_result = doca_sync_event_destroy(resources->comp_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy comp_event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_dpa:
#ifdef DOCA_ARCH_DPU
	tmp_result = doca_dpa_destroy(resources->rdma_doca_dpa);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy extended DOCA DPA context: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
#endif
	tmp_result = doca_dpa_destroy(resources->pf_doca_dpa);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy base DOCA DPA context: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	tmp_result = doca_dev_close(resources->pf_doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close base DOCA DPA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
#ifdef DOCA_ARCH_DPU
	tmp_result = doca_dev_close(resources->rdma_doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close extended DOCA DPA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
#endif

	return result;
}

doca_error_t dpa_a2a_destroy(struct a2a_resources *resources)
{
	doca_error_t result = DOCA_SUCCESS, tmp_result = DOCA_SUCCESS;
	int i;

	/* Free DPA device memory*/

	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_kernel_events_handle);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_rp_remote_kernel_events);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	for (i = 0; i < resources->num_ranks; i++) {
		if (resources->rp_kernel_events[i] != NULL) {
			tmp_result = doca_sync_event_remote_net_destroy(resources->rp_kernel_events[i]);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy remote net DOCA sync event: %s",
					     doca_error_get_descr(tmp_result));
				DOCA_ERROR_PROPAGATE(result, tmp_result);
			}
		}
	}
	free(resources->rp_kernel_events_dpa_handles);
	free(resources->rp_kernel_events);
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_recvbufs);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_recvbufs_mmap_handles);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	free(resources->export_mmaps_dpa_handle);
	for (i = 0; i < resources->num_ranks; i++) {
		if (resources->export_mmaps[i] != NULL) {
			tmp_result = doca_mmap_destroy(resources->export_mmaps[i]);
			if (tmp_result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
				DOCA_ERROR_PROPAGATE(result, tmp_result);
			}
		}
	}
	free(resources->export_mmaps);
	tmp_result = doca_mmap_destroy(resources->recvbuf_mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	tmp_result = doca_mmap_destroy(resources->sendbuf_mmap);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA mmap: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	tmp_result = doca_dpa_mem_free(resources->rdma_doca_dpa, resources->devptr_rdmas);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to free DOCA DPA device memory: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Destroy DOCA DPA RDMAs*/
	for (i = 0; i < resources->num_ranks; i++) {
		tmp_result = destroy_rdma(resources->rdmas[i], resources->rdma_doca_device);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA rdma instance: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->rdmas);

	/* Destroy DOCA DPA completions */
	for (i = 0; i < resources->num_ranks; i++) {
		tmp_result = doca_dpa_completion_destroy(resources->dpa_completions[i]);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA DPA completion instance: %s",
				     doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->dpa_completions);

	/* Free kernel events handles */
	free(resources->kernel_events_handle);
	/* Destroy DOCA DPA kernel events */
	for (i = 0; i < resources->num_ranks; i++) {
		tmp_result = doca_sync_event_destroy(resources->kernel_events[i]);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy kernel_event: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
	free(resources->kernel_events);

	/* Destroy DOCA DPA completion event */
	tmp_result = doca_sync_event_destroy(resources->comp_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy comp_event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Destroy DOCA DPA context */
#ifdef DOCA_ARCH_DPU
	if (resources->rdma_doca_dpa != resources->pf_doca_dpa) {
		tmp_result = doca_dpa_destroy(resources->rdma_doca_dpa);
		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to destroy DOCA DPA context: %s", doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
#endif

	tmp_result = doca_dpa_destroy(resources->pf_doca_dpa);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy DOCA DPA context: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	/* Close DOCA device */
#ifdef DOCA_ARCH_DPU
	tmp_result = doca_dev_close(resources->rdma_doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
#endif

	tmp_result = doca_dev_close(resources->pf_doca_device);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to close DOCA device: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}

	return result;
}

doca_error_t dpa_a2a_req_finalize(struct dpa_a2a_request *req)
{
	doca_error_t result;

	if (req->resources == NULL)
		return DOCA_SUCCESS;

	result = dpa_a2a_destroy(req->resources);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy a2a resources: %s", doca_error_get_descr(result));
		return result;
	}
	free(req->resources);
	req->resources = NULL;

	return result;
}

doca_error_t dpa_a2a_req_wait(struct dpa_a2a_request *req)
{
	doca_error_t result;
	uint64_t se_val;
	double elapsed_time_in_sec = 0;
	struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = SLEEP_IN_NANO_SEC,
	};
	double sleep_in_sec = (double)SLEEP_IN_NANO_SEC / 1000000000;

	if (req->resources == NULL) {
		DOCA_LOG_ERR("Failed to wait for completion event, resourced uninitialized");
		return DOCA_ERROR_UNEXPECTED;
	}

	while (1) {
		result = doca_sync_event_get(req->resources->comp_event, &se_val);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to get completion event value: %s", doca_error_get_descr(result));
			break;
		}

		if (se_val > (req->resources->a2a_seq_num - 1)) {
			result = DOCA_SUCCESS;
			break;
		}

		if (elapsed_time_in_sec > MAX_MPI_WAIT_TIME) {
			result = DOCA_ERROR_TIME_OUT;
			DOCA_LOG_ERR("Timeout polling completion event");
			break;
		}

		nanosleep(&ts, &ts);
		elapsed_time_in_sec += sleep_in_sec;
	}

	return result;
}

doca_error_t dpa_ialltoall(void *sendbuf,
			   int sendcount,
			   MPI_Datatype sendtype,
			   void *recvbuf,
			   int recvcount,
			   MPI_Datatype recvtype,
			   MPI_Comm comm,
			   struct dpa_a2a_request *req)
{
	int num_ranks, my_rank;
	/* Number of threads to run the kernel */
	unsigned int num_threads;
	doca_error_t result;

	/* If current process is not part of any communicator then exit */
	if (comm == MPI_COMM_NULL)
		return DOCA_SUCCESS;

	/* Get the rank of the current process */
	MPI_Comm_rank(comm, &my_rank);
	/* Get the number of processes */
	MPI_Comm_size(comm, &num_ranks);
	if (!req->resources) {
		req->resources = (struct a2a_resources *)calloc(1, sizeof(*(req->resources)));
		if (req->resources == NULL) {
			DOCA_LOG_ERR("Failed to allocate a2a resources");
			return DOCA_ERROR_NO_MEMORY;
		}
		/* Initialize all to all resources */
		req->resources->a2a_seq_num = 0;
		req->resources->comm = comm;
		req->resources->mesg_count = sendcount;
		req->resources->msg_type = sendtype;
		req->resources->my_rank = my_rank;
		req->resources->num_ranks = num_ranks;
		req->resources->sendbuf = sendbuf;
		req->resources->recvbuf = recvbuf;
		result = dpa_a2a_init(req->resources);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to initialize alltoall resources: %s", doca_error_get_descr(result));
			free(req->resources);
			return result;
		}
	}

	/* The number of threads should be the minimum between the number of processes and the maximum number of threads
	 */
	num_threads = (req->resources->num_ranks < MAX_NUM_THREADS) ? req->resources->num_ranks : MAX_NUM_THREADS;

	/* Increment the sequence number */
	req->resources->a2a_seq_num++;

	/* Launch all to all kernel*/
	result = doca_dpa_kernel_launch_update_set(req->resources->pf_doca_dpa,
						   NULL,
						   0,
						   req->resources->comp_event,
						   req->resources->a2a_seq_num,
						   num_threads,
						   &alltoall_kernel,
						   req->resources->rdma_doca_dpa_handle,
						   req->resources->devptr_rdmas,
						   (uint64_t)(req->resources->sendbuf),
						   req->resources->sendbuf_dpa_mmap_handle,
						   (uint64_t)sendcount,
						   (uint64_t)req->resources->extent,
						   (uint64_t)num_ranks,
						   (uint64_t)my_rank,
						   (uint64_t)(req->resources->devptr_recvbufs),
						   (uint64_t)(req->resources->devptr_recvbufs_mmap_handles),
						   req->resources->devptr_kernel_events_handle,
						   req->resources->devptr_rp_remote_kernel_events,
						   req->resources->a2a_seq_num);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to launch alltoall kernel: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t dpa_alltoall(void *sendbuf,
			  int sendcount,
			  MPI_Datatype sendtype,
			  void *recvbuf,
			  int recvcount,
			  MPI_Datatype recvtype,
			  MPI_Comm comm)
{
	struct dpa_a2a_request req = {.resources = NULL};
	doca_error_t result;

	/* Run DPA All to All non-blocking */
	result = dpa_ialltoall(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm, &req);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpa_ialltoall() failed: %s", doca_error_get_descr(result));
		return result;
	}

	/* Wait till the DPA All to All finishes */
	result = dpa_a2a_req_wait(&req);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpa_a2a_req_wait() failed: %s", doca_error_get_descr(result));
		return result;
	}

	/* Wait until all processes finish waiting */
	MPI_Barrier(comm);

	/* Finalize the request */
	result = dpa_a2a_req_finalize(&req);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("dpa_a2a_req_finalize() failed: %s", doca_error_get_descr(result));
		return result;
	}

	return result;
}

doca_error_t dpa_a2a(int argc, char **argv, struct a2a_config *cfg)
{
	int my_rank, num_ranks, i;
	size_t buff_size, msg_size, msg_count;
	int *send_buf, *recv_buf, *send_buf_all, *recv_buf_all;
	MPI_Request reqs[2];
	doca_error_t result;

	/* Initialize MPI variables */
	MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

	if (num_ranks > MAX_NUM_PROC) {
		if (my_rank == 0)
			DOCA_LOG_ERR("Invalid number of processes. Maximum number of processes is %d", MAX_NUM_PROC);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/*
	 * Define message size, message count and buffer size
	 * If it's the default then the message size is the number of processes times size of one integer
	 */
	if (cfg->msgsize == MESSAGE_SIZE_DEFAULT_LEN)
		msg_size = num_ranks * sizeof(int);
	else
		msg_size = (size_t)cfg->msgsize;
	msg_count = (msg_size / num_ranks) / sizeof(int);
	if (msg_count == 0) {
		if (my_rank == 0)
			DOCA_LOG_ERR("Message size %lu too small for the number of processes. Should be at least %lu",
				     msg_size,
				     num_ranks * sizeof(int));
		return DOCA_ERROR_INVALID_VALUE;
	}

	buff_size = msg_size / sizeof(int);

	/* Set devices names */
	strcpy(pf_device1_name, cfg->pf_device1_name);
	if (strncmp(cfg->pf_device2_name, IB_DEVICE_DEFAULT_NAME, strlen(IB_DEVICE_DEFAULT_NAME)) != 0)
		strcpy(pf_device2_name, cfg->pf_device2_name);
	else
		strcpy(pf_device2_name, cfg->pf_device1_name);

	strcpy(rdma_device1_name, cfg->rdma_device1_name);
	if (strncmp(cfg->rdma_device2_name, IB_DEVICE_DEFAULT_NAME, strlen(IB_DEVICE_DEFAULT_NAME)) != 0)
		strcpy(rdma_device2_name, cfg->rdma_device2_name);
	else
		strcpy(rdma_device2_name, cfg->rdma_device1_name);

	if (my_rank == 0)
		DOCA_LOG_INFO("Number of processes = %d, message size = %lu, message count = %lu, buffer size = %lu",
			      num_ranks,
			      msg_size,
			      msg_count,
			      buff_size);

	/* Allocate and initialize the buffers */
	send_buf = calloc(buff_size, sizeof(int));
	recv_buf = calloc(buff_size, sizeof(int));
	send_buf_all = calloc(num_ranks * buff_size, sizeof(int));
	recv_buf_all = calloc(num_ranks * buff_size, sizeof(int));

	if (send_buf == NULL || recv_buf == NULL || send_buf_all == NULL || recv_buf_all == NULL) {
		DOCA_LOG_ERR("Failed to allocate memory for send/recv buffers");
		result = DOCA_ERROR_NO_MEMORY;
		goto destroy_bufs;
	}

	/* Seed srand */
	srand(time(NULL) + my_rank);
	for (i = 0; i < buff_size; i++)
		send_buf[i] = compute_random_int();

	MPI_Barrier(MPI_COMM_WORLD);

	/* Perform DPA All to All */
	result = dpa_alltoall(send_buf, msg_count, MPI_INT, recv_buf, msg_count, MPI_INT, MPI_COMM_WORLD);
	if (result != DOCA_SUCCESS) {
		if (my_rank == 0)
			DOCA_LOG_ERR("DPA MPI alltoall failed: %s", doca_error_get_descr(result));
		goto destroy_bufs;
	}

	/* Receive all the sendbuf and the recvbuf from all the processes to print */
	MPI_Iallgather(send_buf, buff_size, MPI_INT, send_buf_all, buff_size, MPI_INT, MPI_COMM_WORLD, &reqs[0]);

	result = mpi_request_wait_timeout(&reqs[0], MAX_MPI_WAIT_TIME);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting on allgather: %s", doca_error_get_descr(result));
		goto destroy_bufs;
	}

	MPI_Iallgather(recv_buf, buff_size, MPI_INT, recv_buf_all, buff_size, MPI_INT, MPI_COMM_WORLD, &reqs[1]);

	result = mpi_request_wait_timeout(&reqs[1], MAX_MPI_WAIT_TIME);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Timed out waiting on allgather: %s", doca_error_get_descr(result));
		goto destroy_bufs;
	}

	if (my_rank == 0) {
		printf("         ------------send buffs----------------------\n");
		print_buff(send_buf_all, buff_size, num_ranks);
		printf("         ------------recv buffs----------------------\n");
		print_buff(recv_buf_all, buff_size, num_ranks);
	}

destroy_bufs:
	free(send_buf);
	free(send_buf_all);
	free(recv_buf);
	free(recv_buf_all);

	return result;
}
