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

#ifndef LIFEGUARD_DEPOSIT_SAMPLE_H_
#define LIFEGUARD_DEPOSIT_SAMPLE_H_

#include <sys/un.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_lifeguard_client.h>

/* Service name passed to doca_lifeguard_connect(). */
#define LIFEGUARD_DEPOSIT_SERVICE_NAME "doca_lifeguard_sample"
/* sockaddr_un.sun_path, including the NUL. */
#define LIFEGUARD_DEPOSIT_SOCKET_PATH_SIZE sizeof(((struct sockaddr_un *)0)->sun_path)

struct lifeguard_deposit_cfg {
	char device[DOCA_DEVINFO_IFACE_NAME_SIZE];	      /* Interface name or PCI address */
	char socket_path[LIFEGUARD_DEPOSIT_SOCKET_PATH_SIZE]; /* Lifeguard Unix socket */
};

/*
 * Connect to lifeguard, recover a previous deposit when one exists, and deposit
 * this process's device until SIGINT or SIGTERM.
 *
 * @cfg [in]: Sample configuration
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t lifeguard_deposit(const struct lifeguard_deposit_cfg *cfg);

#endif /* LIFEGUARD_DEPOSIT_SAMPLE_H_ */
