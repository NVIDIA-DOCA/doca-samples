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

#include <doca_dpa_dev.h>
#include <doca_dpa_dev_verbs.h>

/*
 * RPC function for receiving packets on the DPA
 *
 * @verbs_eth_rq_handle [in]: The Verbs ETH RQ handle
 * @dpa_completion_handle [in]: The DPA completion handle
 * @buf [in]: The buffer to receive packets into
 * @mkey [in]: The MKey
 * @packets_number [in]: The number of packets to receive
 * @max_packet_size [in]: The maximum size of each packet
 * @return: RPC function always succeed and returns 0
 */
__dpa_rpc__ uint64_t receive_packets_rpc(doca_dpa_dev_verbs_eth_rq_t verbs_eth_rq_handle,
					 doca_dpa_dev_completion_t dpa_completion_handle,
					 doca_dpa_dev_uintptr_t buf,
					 uint32_t mkey,
					 uint32_t packets_number,
					 uint32_t max_packet_size)
{
	uint8_t *packet_buf = (uint8_t *)buf;
	struct doca_dpa_dev_verbs_sge sge;
	struct doca_dpa_dev_verbs_recv_wr recv_wr;
	doca_dpa_dev_completion_element_t completion;

	for (uint32_t i = 0; i < packets_number; i++) {
		sge.addr = (uint64_t)packet_buf;
		sge.length = max_packet_size;
		sge.lkey = mkey;

		doca_dpa_dev_verbs_recv_wr_set_sg_list(&recv_wr, &sge);
		doca_dpa_dev_verbs_recv_wr_set_sg_num_sge(&recv_wr, 1);

		doca_dpa_dev_verbs_eth_rq_post_recv_wr(verbs_eth_rq_handle, &recv_wr);
		packet_buf += max_packet_size;
	}

	doca_dpa_dev_verbs_eth_rq_commit_recv(verbs_eth_rq_handle);

	for (uint32_t i = 0; i < packets_number; i++) {
		while (!doca_dpa_dev_get_completion(dpa_completion_handle, &completion))
			;
		DOCA_DPA_DEV_LOG_INFO("Received completion for packet %u with: wqe counter %d, timestamp %lu\n",
				      i,
				      doca_dpa_dev_completion_element_get_wqe_counter(completion),
				      doca_dpa_dev_completion_element_get_timestamp(completion));
	}

	doca_dpa_dev_completion_ack(dpa_completion_handle, packets_number);

	return 0;
}
