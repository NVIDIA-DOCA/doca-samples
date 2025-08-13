/*
 * Copyright (c) 2021 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef SIMPLE_FWD_PORT_H_
#define SIMPLE_FWD_PORT_H_

#define NUM_OF_PORTS (2) /* Number of ports used */

/* Simple FWD application's port configuration */
struct simple_fwd_port_cfg {
	uint16_t port_id;     /* Port identifier for the application */
	uint16_t nb_queues;   /* Number of initialized queues descriptors (RX/TX) of the port */
	uint32_t nb_meters;   /* Number of meters of the port used by the application */
	uint32_t nb_counters; /* Number of counters for the port used by the application */
	bool is_hairpin;      /* Number of hairpin queues */
	bool age_thread;      /* Whether or not aging is handled by a dedicated thread */
};

/*
 * Dump port stats
 *
 * @port_id [in]: Port identifier
 * @return: 0 on success and non-zero value on failure
 */
int simple_fwd_dump_port_stats(uint16_t port_id);

#endif /* SIMPLE_FWD_PORT_H_ */
