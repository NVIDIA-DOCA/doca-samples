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

#ifndef ETH_RXQ_COMMON_H_
#define ETH_RXQ_COMMON_H_

#include <unistd.h>

#include <doca_error.h>
#include <doca_buf.h>

/*
 * Get DOCA buf packet headroom size for ETH RXQ sample
 *
 * @pkt [in]: DOCA buf packet for ETH RXQ
 * @headroom_size [out]: packet headroom size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t get_pkt_headroom(struct doca_buf *pkt, uint16_t *headroom_size);

/*
 * Get DOCA buf packet tailroom size for ETH RXQ sample
 *
 * @pkt [in]: DOCA buf packet for ETH RXQ
 * @tailroom_size [out]: packet tailroom size
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */
doca_error_t get_pkt_tailroom(struct doca_buf *pkt, uint16_t *tailroom_size);

#endif /* ETH_RXQ_COMMON_H_ */
