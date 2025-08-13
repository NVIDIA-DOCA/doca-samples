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

#ifndef RTT_TEMPLATE_CTXT_H_
#define RTT_TEMPLATE_CTXT_H_

typedef struct {
	uint8_t was_nack : 1; /* Signal the reception of a NACK */
	uint8_t was_cnp : 1;  /* Signal the reception of a CNP */
	uint8_t reserved : 6; /* Reserved bits */
} rtt_template_flags_t;

typedef struct {
	uint32_t cur_rate;	     /* Current rate */
	uint32_t start_delay;	     /* The time at which the RTT packet was sent by the NIC's Tx pipe */
	uint32_t rtt;		     /* Value of the last measured round trip time */
	rtt_template_flags_t flags;  /* Flags struct */
	uint8_t abort_cnt;	     /* Counter of abort RTT requests */
	uint8_t rtt_meas_psn;	     /* RTT request sequence number */
	uint8_t rtt_req_to_rtt_sent; /* Set between the algorithm's RTT request until the time at which the RTT packet
					was sent */
	uint32_t reserved[8];	     /* Reserved bits */
} cc_ctxt_rtt_template_t;

#endif /* RTT_TEMPLATE_CTXT_H_ */
