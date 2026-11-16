/*
 * Copyright (c) 2023-2026 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

#include <doca_gpunetio_dev_eth_txq.cuh>
#include <doca_log.h>

#include "../gpunetio_common.h"

DOCA_LOG_REGISTER(GPU_SEND_WAIT_TIME::KERNEL);

/* Every burst takes a wait WQE plus one send WQE per packet, and all the bursts are queued at once */
static_assert(NUM_BURST_SEND * (NUM_PACKETS_X_BURST + 1) <= MAX_SQ_DESCR_NUM, "All the bursts must fit in the Txq");

/*
 * Send NUM_BURST_SEND bursts of blockDim.x packets, every burst at its own time. Every thread sends one packet of
 * each burst.
 *
 * @txq [in]: DOCA Eth Tx queue GPU handler
 * @pkt_buff_addr [in]: packet buffer, NUM_BURST_SEND bursts of blockDim.x packets
 * @pkt_buff_mkey [in]: packet buffer memory key, in network byte order
 * @pkt_size [in]: size of every packet
 * @wait_times [in]: wait on time value of every burst
 * @bursts_done [in/out]: number of bursts sent without error, set to 0 by the caller
 */
__global__ void send_wait_on_time(struct doca_gpu_eth_txq *txq,
				  uint8_t *pkt_buff_addr,
				  const uint32_t pkt_buff_mkey,
				  const size_t pkt_size,
				  const struct burst_wait_times wait_times,
				  uint32_t *bursts_done)
{
	doca_error_t status;
	doca_gpu_dev_eth_ticket_t out_ticket;
	enum doca_gpu_eth_send_flags flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NONE;
	uint64_t addr;
	uint32_t num_completed;
	const uint64_t burst_size = pkt_size * blockDim.x;

	/* Only the last packet of a burst requests a CQE, which also completes the packets before it */
	if (threadIdx.x == (blockDim.x - 1))
		flags = DOCA_GPUNETIO_ETH_SEND_FLAG_NOTIFY;

	/*
	 * The wait WQE in front of every burst makes the NIC hold the burst until its time, so all the bursts are
	 * queued at once. Waiting for the completion of a burst before posting the next one would delay the next
	 * burst whenever the interval is short.
	 * Only this block uses the Txq, so CTA resource sharing is enough. The NIC must see the WQEs before the
	 * doorbell, which takes the GPU sync scope.
	 */
	for (uint32_t idx = 0; idx < NUM_BURST_SEND; idx++) {
		addr = (uint64_t)pkt_buff_addr + burst_size * idx + (uint64_t)pkt_size * threadIdx.x;
		doca_gpu_dev_eth_txq_wait_send<DOCA_GPUNETIO_ETH_RESOURCE_SHARING_MODE_CTA,
					       DOCA_GPUNETIO_ETH_SYNC_SCOPE_GPU,
					       DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
					       DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK>(txq,
										   wait_times.wait_on_time[idx],
										   addr,
										   pkt_buff_mkey,
										   pkt_size,
										   flags,
										   &out_ticket);
	}

	/* One CQE per burst, in order: report how many bursts were sent before the first error, if any */
	if (threadIdx.x == 0) {
		for (uint32_t idx = 0; idx < NUM_BURST_SEND; idx++) {
			status = doca_gpu_dev_eth_txq_poll_completion<DOCA_GPUNETIO_ETH_CQ_POLL_LAST>(
				txq,
				1,
				DOCA_GPUNETIO_ETH_WAIT_FLAG_B,
				&num_completed);
			if (status != DOCA_SUCCESS)
				break;
			DOCA_GPUNETIO_VOLATILE(*bursts_done) = idx + 1;
		}
	}
}

extern "C" {

doca_error_t kernel_send_wait_on_time(cudaStream_t stream,
				      struct txq_queue *txq,
				      const struct burst_wait_times *wait_times,
				      uint32_t *bursts_done_gpu)
{
	cudaError_t result = cudaSuccess;

	if (txq == NULL || wait_times == NULL || bursts_done_gpu == NULL) {
		DOCA_LOG_ERR("kernel_send_wait_on_time invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (txq->pkt_num < NUM_PACKETS_X_BURST * NUM_BURST_SEND) {
		DOCA_LOG_ERR("The packet buffer holds %u packets, %d are required",
			     txq->pkt_num,
			     NUM_PACKETS_X_BURST * NUM_BURST_SEND);
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	/* For simplicity, 1 thread per packet in burst */
	send_wait_on_time<<<1, NUM_PACKETS_X_BURST, 0, stream>>>(txq->eth_txq_gpu,
								 txq->pkt_buff_addr,
								 txq->pkt_buff_mkey,
								 txq->pkt_size,
								 *wait_times,
								 bursts_done_gpu);
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
