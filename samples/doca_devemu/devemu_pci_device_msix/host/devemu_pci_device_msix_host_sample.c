/*
 * Copyright (c) 2024-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <errno.h>
#include <signal.h>
#include <linux/vfio.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>

#include <common.h>
#include <devemu_pci_common.h>
#include <devemu_pci_host_common.h>

DOCA_LOG_REGISTER(DEVEMU_PCI_DEVICE_MSIX_HOST);

static bool force_quit; /* Shared variable to allow for a proper shutdown */

/*
 * Signal handler
 *
 * @signum [in]: Signal number to handle
 */
static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		DOCA_LOG_INFO("Signal %d received, preparing to exit", signum);
		force_quit = true;
	}
}

/*
 * Create an eventfd for each MSI-X vector
 *
 * @resources [in]: The sample resources
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
static doca_error_t map_msix_to_fds(struct devemu_host_resources *resources)
{
	int *irq_set_data;
	uint32_t idx;
	doca_error_t result;
	struct vfio_irq_info irq_info = {
		.argsz = sizeof(irq_info),
		.index = VFIO_PCI_MSIX_IRQ_INDEX,
	};
	int status = ioctl(resources->device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
	if (status != 0) {
		DOCA_LOG_ERR("Failed to get IRQ set %u info. Status=%d, errno=%d",
			     VFIO_PCI_MSIX_IRQ_INDEX,
			     status,
			     errno);
		return DOCA_ERROR_DRIVER;
	}
	if (irq_info.count == 0) {
		DOCA_LOG_ERR("Device does not have MSI-X capability");
		return DOCA_ERROR_INVALID_VALUE;
	}

	const size_t fd_table_size = (sizeof(int) * irq_info.count);
	resources->msix_vector_to_fd = malloc(fd_table_size);
	if (resources->msix_vector_to_fd == NULL) {
		DOCA_LOG_ERR("Failed to allocate msix to fd table, with %u entries", irq_info.count);
		return DOCA_ERROR_NO_MEMORY;
	}

	struct vfio_irq_set *irq_set = malloc(sizeof(struct vfio_irq_set) + fd_table_size);
	if (irq_set == NULL) {
		DOCA_LOG_ERR("Failed to allocate vfio_irq_set, with space for %u fds", irq_info.count);
		result = DOCA_ERROR_NO_MEMORY;
		goto cleanup_fd_table;
	}
	irq_set->argsz = sizeof(struct vfio_irq_set) + fd_table_size;
	irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = irq_info.index;
	irq_set->start = 0;
	irq_set->count = irq_info.count;

	irq_set_data = (int *)&irq_set->data[0];
	for (idx = 0; idx < irq_set->count; ++idx) {
		resources->msix_vector_to_fd[idx] = eventfd(0, EFD_NONBLOCK);
		if (resources->msix_vector_to_fd[idx] == -1) {
			DOCA_LOG_ERR("Failed to create eventfd for MSI-X index %u", idx);
			result = DOCA_ERROR_DRIVER;
			goto cleanup_irq_set;
		}

		irq_set_data[idx] = resources->msix_vector_to_fd[idx];
	}

	status = ioctl(resources->device_fd, VFIO_DEVICE_SET_IRQS, irq_set);
	if (status != 0) {
		DOCA_LOG_ERR("Failed to set MSI-X IRQs. Status=%d, errno=%d", status, errno);
		result = DOCA_ERROR_DRIVER;
		goto cleanup_irq_set;
	}
	free(irq_set);

	return DOCA_SUCCESS;

cleanup_irq_set:
	free(irq_set);
cleanup_fd_table:
	free(resources->msix_vector_to_fd);
	resources->msix_vector_to_fd = NULL;

	return result;
}

/*
 * Check if any interrupt was received on the MSI-X vector
 *
 * @resources [in]: The sample resources
 * @msix_idx [in]: The MSI-X vector index
 */
static void read_msix_events(struct devemu_host_resources *resources, uint16_t msix_idx)
{
	uint64_t value;
	ssize_t size = read(resources->msix_vector_to_fd[msix_idx], &value, sizeof(value));
	if (size == -1) {
		if (errno != EAGAIN)
			DOCA_LOG_ERR("Received error while reading MSI-X vector index %u. errno %d", msix_idx, errno);
		return;
	}

	DOCA_LOG_INFO("Event received for MSI-X vector index %u new value %ld", msix_idx, value);
}

/*
 * Run DOCA Device Emulation MSI-X Host sample
 *
 * @pci_address [in]: Emulated device PCI address
 * @vfio_group [in]: VFIO group ID
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t devemu_pci_device_msix_host(const char *pci_address, int vfio_group)
{
	doca_error_t result;
	struct devemu_host_resources resources = {0};

	/* Signal the while loop to stop */
	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	resources.container_fd = -1;
	resources.group_fd = -1;
	resources.device_fd = -1;

	result = init_vfio_device(&resources, vfio_group, pci_address);
	if (result != DOCA_SUCCESS) {
		devemu_host_resources_cleanup(&resources);
		return result;
	}

	result = map_msix_to_fds(&resources);
	if (result != DOCA_SUCCESS) {
		devemu_host_resources_cleanup(&resources);
		return result;
	}

	DOCA_LOG_INFO("Listening on all MSI-X vectors");

	while (!force_quit)
		for (uint16_t msix = 0; msix < PCI_TYPE_NUM_MSIX; msix++)
			read_msix_events(&resources, msix);

	devemu_host_resources_cleanup(&resources);

	return DOCA_SUCCESS;
}
