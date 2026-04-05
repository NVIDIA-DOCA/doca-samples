/*
 * Copyright (c) 2024-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#ifndef _TELEM_TEMPLATE_ALGO_PARAMS_H_
#define _TELEM_TEMPLATE_ALGO_PARAMS_H_

/* Configurable algorithm parameters */
/* This parameters are hardcoded and they provide the best set of the parameters for real firmware */
#define UPDATE_FACTOR (((1 << 16) * 10) / 100) /* 0.08 in fxp16 - maximum multiplicative decrease factor */
#define BASE_RTT (13000)		       /* Base value of rtt - in nanosec */
#define NEW_FLOW_RATE (1 << (20))	       /* Rate format in fixed point 20 */
#define MIN_RATE (1 << (20 - 14))	       /* Rate format in fixed point 20 */
#define MAX_DELAY (150000)		       /* Maximum delay - in nanosec */

#define UPDATE_FACTOR_MAX (10 * (1 << 16)) /* Maximum value of update factor */
#define AI_MAX (1 << (20))		   /* Maximum value of AI */
#define RATE_MAX (1 << (20))		   /* Maximum value of rate */

#define AI (40) // in bytes
#define HAI (2 << (20 - 8))
#define BASE_QLEN (200000) // in Bytes
#define PORT_BW_G (200)

#endif /* _TELEM_TEMPLATE_ALGO_PARAMS_H_ */
