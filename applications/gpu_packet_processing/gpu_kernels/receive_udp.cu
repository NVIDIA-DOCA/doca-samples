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

#include <stdlib.h>
#include <string.h>

#include <doca_gpunetio_dev_buf.cuh>
#include <doca_gpunetio_dev_sem.cuh>
#include <doca_gpunetio_dev_eth_rxq.cuh>

#include "common.h"
#include "packets.h"
#include "filters.cuh"

DOCA_LOG_REGISTER(GPU_SANITY::KERNEL_RECEIVE_UDP);

static_assert(sizeof(struct doca_gpu_dev_eth_rxq_attr) == RX_ATTR_BYTES, "RX_ATTR_BYTES mismatch");
static_assert(sizeof(struct stats_udp) % 64 == 0, "struct stats_udp size must be a multiple of 64 bytes");
static_assert(alignof(struct stats_udp) == 64, "struct stats_udp must be 64 bytes aligned");

/* UDP receive CUDA kernel parameters */
struct udp_kernel_params {
	uint32_t *exit_cond;				       /* Exit condition set by the CPU */
	struct doca_gpu_eth_rxq *rxq[MAX_QUEUES];	       /* Receive queue, one per CUDA block */
	struct doca_gpu_semaphore_gpu *sem[MAX_QUEUES];	       /* Stats semaphore, one per CUDA block */
	struct doca_gpu_dev_eth_rxq_attr *rx_attr[MAX_QUEUES]; /* Per-packet CQE attributes, one per CUDA block */
	int sem_num;					       /* Number of items per semaphore */
	uint32_t max_rx_pkts;				       /* Max packets per receive call, 0 means no limit */
	uint64_t timeout_ns;				       /* Receive call timeout */
};

static __device__ __forceinline__ void latency_reset(struct stats_latency *lat)
{
	lat->num = 0;
	lat->sum = 0;
	lat->min = INT64_MAX;
	lat->max = INT64_MIN;
}

static __device__ __forceinline__ void latency_add(struct stats_latency *lat, int64_t sample)
{
	lat->num++;
	lat->sum += sample;
	lat->min = sample < lat->min ? sample : lat->min;
	lat->max = sample > lat->max ? sample : lat->max;
}

/*
 * Each CUDA block receives from its own queue, and its size matches rx_scope (see udp_rx_scope_threads): one thread,
 * one warp or CUDA_THREADS threads. So every thread, warp or block works on a different queue; all its threads call
 * the receive function and process the received packets.
 *
 * stage_latency: measure with the GPU global timer the duration of each kernel stage (receive, processing, report)
 * of the iterations receiving packets.
 * packet_filtering: process every received packet (DNS filter, packet wipe) and report DNS/other packets and bytes. If
 * false (receive-only benchmark) the kernel reads only the IP total length of each packet to report total packets and
 * bytes.
 * cqe_attr_check: receive with all the CQE attributes and validate them for every packet: bytes must match the packet
 * frame length, timestamp must be non-zero and non-decreasing within a receive. Checked attributes are cleared, so an
 * entry not written by the next receive (a hole) is reported as a bytes error.
 */
template <enum doca_gpu_dev_eth_exec_scope rx_scope, bool stage_latency, bool packet_filtering, bool cqe_attr_check>
__global__ void cuda_kernel_receive_udp(const struct udp_kernel_params params)
{
	constexpr enum doca_gpu_dev_eth_rxq_attr_type rx_attr = cqe_attr_check ? DOCA_GPUNETIO_ETH_RX_ATTR_ALL :
										 DOCA_GPUNETIO_ETH_RX_ATTR_NONE;

	__shared__ uint64_t out_first_pkt_idx;
	__shared__ uint32_t out_pkt_num;
	__shared__ doca_error_t rx_status;
	__shared__ uint32_t exit_loop;
	__shared__ struct stats_udp stats_sh;

	doca_error_t ret;
	struct doca_gpu_eth_rxq *rxq;
	struct doca_gpu_semaphore_gpu *sem;
	struct doca_gpu_dev_eth_rxq_attr *out_attr;
	struct doca_gpu_dev_eth_rxq_attr attr;
	struct stats_udp stats_thread;
	struct stats_udp *stats_global;
	struct stats_latency lat_recv, lat_process, lat_report;
	struct eth_ip_udp_hdr *hdr;
	uintptr_t buf_addr;
	uint64_t first_pkt_idx;
	uint64_t recv_pkts = 0;
	uint64_t t_start = 0, t_recv = 0, t_process = 0;
	uint32_t buf_idx, pkt_num;
	uint32_t lane_id = doca_gpu_dev_eth_get_lane_id();
	uint32_t sem_idx = 0;
	uint8_t *payload;
	uint16_t pkt_len;

	if (blockIdx.x >= MAX_QUEUES)
		return;

	rxq = params.rxq[blockIdx.x];
	sem = params.sem[blockIdx.x];
	out_attr = params.rx_attr[blockIdx.x];

	if (threadIdx.x == 0) {
		DOCA_GPUNETIO_VOLATILE(stats_sh.dns) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.dns_bytes) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.others) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.others_bytes) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.total_bytes) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.attr_checked) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.attr_bytes_err) = 0;
		DOCA_GPUNETIO_VOLATILE(stats_sh.attr_ts_err) = 0;
		DOCA_GPUNETIO_VOLATILE(exit_loop) = DOCA_GPUNETIO_VOLATILE(*params.exit_cond);
	}
	latency_reset(&lat_recv);
	latency_reset(&lat_process);
	latency_reset(&lat_report);
	__syncthreads();

	/* exit_loop is read by thread 0 only, so all the block threads leave the loop at the same iteration */
	while (DOCA_GPUNETIO_VOLATILE(exit_loop) == 0) {
		stats_thread.dns = 0;
		stats_thread.others = 0;
		stats_thread.dns_bytes = 0;
		stats_thread.others_bytes = 0;
		stats_thread.total_bytes = 0;
		stats_thread.attr_checked = 0;
		stats_thread.attr_bytes_err = 0;
		stats_thread.attr_ts_err = 0;

		if (threadIdx.x == 0) {
			DOCA_GPUNETIO_VOLATILE(rx_status) = DOCA_SUCCESS;
			if (stage_latency)
				t_start = doca_gpu_dev_eth_query_globaltimer();
		}

		ret = doca_gpu_dev_eth_rxq_recv<rx_scope,
						DOCA_GPUNETIO_ETH_MCST_AUTO,
						DOCA_GPUNETIO_ETH_NIC_HANDLER_AUTO,
						rx_attr>(rxq,
							 params.max_rx_pkts,
							 params.timeout_ns,
							 &out_first_pkt_idx,
							 &out_pkt_num,
							 out_attr);
		if (ret != DOCA_SUCCESS)
			DOCA_GPUNETIO_VOLATILE(rx_status) = ret;

		if (stage_latency && threadIdx.x == 0)
			t_recv = doca_gpu_dev_eth_query_globaltimer();
		/* Make receive output visible to all the block threads */
		__syncthreads();

		/* If any thread returns receive error, the whole execution stops */
		if (DOCA_GPUNETIO_VOLATILE(rx_status) != DOCA_SUCCESS) {
			if (threadIdx.x == 0) {
#if ENABLE_KERNEL_DEBUG_PRINTS == 1
				printf("Receive UDP kernel error %d Block %d\n", rx_status, blockIdx.x);
#endif
				DOCA_GPUNETIO_VOLATILE(*params.exit_cond) = 1;
			}
			break;
		}

		pkt_num = DOCA_GPUNETIO_VOLATILE(out_pkt_num);
		first_pkt_idx = DOCA_GPUNETIO_VOLATILE(out_first_pkt_idx);

		if (pkt_num > 0) {
			if (stage_latency && threadIdx.x == 0) {
				latency_add(&lat_recv, (int64_t)(t_recv - t_start));
				recv_pkts += pkt_num;
			}

			buf_idx = threadIdx.x;
			while (buf_idx < pkt_num) {
				buf_addr = doca_gpu_dev_eth_rxq_get_pkt_addr(rxq, first_pkt_idx + buf_idx);
				raw_to_udp(buf_addr, &hdr, &payload);
				pkt_len = BYTE_SWAP16(hdr->l3_hdr.total_length) + sizeof(struct ether_hdr);
				stats_thread.total_bytes += pkt_len;

				if (cqe_attr_check) {
					attr = out_attr[buf_idx];
					stats_thread.attr_checked++;
					/* Shorter frames are padded to the Ethernet minimum length */
					if (attr.bytes != pkt_len &&
					    !(pkt_len < ETHER_MIN_FRAME_LEN && attr.bytes == ETHER_MIN_FRAME_LEN))
						stats_thread.attr_bytes_err++;
					if (attr.timestamp_ns == 0)
						stats_thread.attr_ts_err++;
				}

				if (packet_filtering) {
					if (filter_is_dns(&(hdr->l4_hdr), payload)) {
						stats_thread.dns++;
						stats_thread.dns_bytes += pkt_len;
					} else {
						stats_thread.others++;
						stats_thread.others_bytes += pkt_len;
					}

					/* Double-proof it's not reading old packets */
					wipe_packet_32b((uint8_t *)&(hdr->l4_hdr));
				}
				buf_idx += blockDim.x;
			}
			__syncthreads();

			/* Clear checked attributes, after all the threads compared them with their predecessor */
			if (cqe_attr_check) {
				for (buf_idx = threadIdx.x; buf_idx < pkt_num; buf_idx += blockDim.x) {
					out_attr[buf_idx].bytes = 0;
					out_attr[buf_idx].timestamp_ns = 0;
				}
			}

			if (stage_latency && threadIdx.x == 0) {
				t_process = doca_gpu_dev_eth_query_globaltimer();
				latency_add(&lat_process, (int64_t)(t_process - t_recv));
			}

			/* Thread scope blocks have a single thread, nothing to reduce within the warp */
#pragma unroll
			for (int offset = 16; rx_scope != DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD && offset > 0;
			     offset /= 2) {
				stats_thread.total_bytes +=
					__shfl_down_sync(WARP_FULL_MASK, stats_thread.total_bytes, offset);
				if (packet_filtering) {
					stats_thread.dns += __shfl_down_sync(WARP_FULL_MASK, stats_thread.dns, offset);
					stats_thread.dns_bytes +=
						__shfl_down_sync(WARP_FULL_MASK, stats_thread.dns_bytes, offset);
					stats_thread.others +=
						__shfl_down_sync(WARP_FULL_MASK, stats_thread.others, offset);
					stats_thread.others_bytes +=
						__shfl_down_sync(WARP_FULL_MASK, stats_thread.others_bytes, offset);
				}
				if (cqe_attr_check) {
					stats_thread.attr_checked +=
						__shfl_down_sync(WARP_FULL_MASK, stats_thread.attr_checked, offset);
					stats_thread.attr_bytes_err +=
						__shfl_down_sync(WARP_FULL_MASK, stats_thread.attr_bytes_err, offset);
					stats_thread.attr_ts_err +=
						__shfl_down_sync(WARP_FULL_MASK, stats_thread.attr_ts_err, offset);
				}
				__syncwarp();
			}

			if (lane_id == 0) {
				atomicAdd_block((unsigned long long *)&(stats_sh.total_bytes),
						stats_thread.total_bytes);
				if (packet_filtering) {
					atomicAdd_block((unsigned long long *)&(stats_sh.dns), stats_thread.dns);
					atomicAdd_block((unsigned long long *)&(stats_sh.dns_bytes),
							stats_thread.dns_bytes);
					atomicAdd_block((unsigned long long *)&(stats_sh.others), stats_thread.others);
					atomicAdd_block((unsigned long long *)&(stats_sh.others_bytes),
							stats_thread.others_bytes);
				}
				if (cqe_attr_check) {
					atomicAdd_block((unsigned long long *)&(stats_sh.attr_checked),
							stats_thread.attr_checked);
					atomicAdd_block((unsigned long long *)&(stats_sh.attr_bytes_err),
							stats_thread.attr_bytes_err);
					atomicAdd_block((unsigned long long *)&(stats_sh.attr_ts_err),
							stats_thread.attr_ts_err);
				}
			}
			__syncthreads();

			if (threadIdx.x == 0) {
				ret = doca_gpu_dev_semaphore_get_custom_info_addr(sem, sem_idx, (void **)&stats_global);
				if (ret != DOCA_SUCCESS) {
#if ENABLE_KERNEL_DEBUG_PRINTS == 1
					printf("UDP Error %d doca_gpu_dev_semaphore_get_custom_info_addr block %d thread %d\n",
					       ret,
					       blockIdx.x,
					       threadIdx.x);
#endif
					DOCA_GPUNETIO_VOLATILE(*params.exit_cond) = 1;
				} else {
					DOCA_GPUNETIO_VOLATILE(stats_global->dns) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.dns);
					DOCA_GPUNETIO_VOLATILE(stats_global->others) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.others);
					DOCA_GPUNETIO_VOLATILE(stats_global->dns_bytes) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.dns_bytes);
					DOCA_GPUNETIO_VOLATILE(stats_global->others_bytes) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.others_bytes);
					DOCA_GPUNETIO_VOLATILE(stats_global->total_bytes) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.total_bytes);
					DOCA_GPUNETIO_VOLATILE(stats_global->total) = pkt_num;
					DOCA_GPUNETIO_VOLATILE(stats_global->attr_checked) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.attr_checked);
					DOCA_GPUNETIO_VOLATILE(stats_global->attr_bytes_err) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.attr_bytes_err);
					DOCA_GPUNETIO_VOLATILE(stats_global->attr_ts_err) =
						DOCA_GPUNETIO_VOLATILE(stats_sh.attr_ts_err);

					/* Stage latencies accumulated since the previous report */
					if (stage_latency) {
						stats_global->recv = lat_recv;
						stats_global->process = lat_process;
						stats_global->report = lat_report;
						stats_global->recv_pkts = recv_pkts;
						latency_reset(&lat_recv);
						latency_reset(&lat_process);
						latency_reset(&lat_report);
						recv_pkts = 0;
					} else {
						stats_global->recv.num = 0;
						stats_global->process.num = 0;
						stats_global->report.num = 0;
						stats_global->recv_pkts = 0;
					}

					/* Stats must be visible to the CPU before the semaphore status */
					__threadfence_system();
					doca_gpu_dev_semaphore_set_status(sem,
									  sem_idx,
									  DOCA_GPU_SEMAPHORE_STATUS_READY);
					__threadfence_system();

					sem_idx = (sem_idx + 1) % params.sem_num;

					DOCA_GPUNETIO_VOLATILE(stats_sh.dns) = 0;
					DOCA_GPUNETIO_VOLATILE(stats_sh.others) = 0;
					DOCA_GPUNETIO_VOLATILE(stats_sh.dns_bytes) = 0;
					DOCA_GPUNETIO_VOLATILE(stats_sh.others_bytes) = 0;
					DOCA_GPUNETIO_VOLATILE(stats_sh.total_bytes) = 0;
					DOCA_GPUNETIO_VOLATILE(stats_sh.attr_checked) = 0;
					DOCA_GPUNETIO_VOLATILE(stats_sh.attr_bytes_err) = 0;
					DOCA_GPUNETIO_VOLATILE(stats_sh.attr_ts_err) = 0;
				}
			}
		}

		if (threadIdx.x == 0)
			DOCA_GPUNETIO_VOLATILE(exit_loop) = DOCA_GPUNETIO_VOLATILE(*params.exit_cond);
		__syncthreads();

		/* Reported with the next stats as it includes the semaphore update */
		if (stage_latency && threadIdx.x == 0 && pkt_num > 0)
			latency_add(&lat_report, (int64_t)(doca_gpu_dev_eth_query_globaltimer() - t_process));
	}
}

/*
 * Number of CUDA threads of each UDP receive block, one block per queue
 *
 * @rx_scope [in]: receive function execution scope
 * @return: CUDA block size
 */
static constexpr uint32_t udp_rx_scope_threads(enum doca_gpu_dev_eth_exec_scope rx_scope)
{
	return rx_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD ? 1 :
	       rx_scope == DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP   ? WARP_SIZE :
								 CUDA_THREADS;
}

/*
 * Launch the UDP receive CUDA kernel instance matching the CQE attribute check option
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @numq [in]: number of queues, one CUDA block per queue
 * @params [in]: kernel parameters
 * @rx_cfg [in]: UDP receive configuration
 */
template <enum doca_gpu_dev_eth_exec_scope rx_scope, bool stage_latency, bool packet_filtering>
static void launch_kernel_receive_udp_attr(cudaStream_t stream,
					   uint16_t numq,
					   const struct udp_kernel_params &params,
					   const struct udp_rx_cfg *rx_cfg)
{
	constexpr uint32_t nthreads = udp_rx_scope_threads(rx_scope);

	if (rx_cfg->cqe_attr_check)
		cuda_kernel_receive_udp<rx_scope, stage_latency, packet_filtering, true>
			<<<numq, nthreads, 0, stream>>>(params);
	else
		cuda_kernel_receive_udp<rx_scope, stage_latency, packet_filtering, false>
			<<<numq, nthreads, 0, stream>>>(params);
}

/*
 * Launch the UDP receive CUDA kernel instance matching the stage latency, packet filtering and CQE attribute check
 * options
 *
 * @stream [in]: CUDA stream to launch the kernel
 * @numq [in]: number of queues, one CUDA block per queue
 * @params [in]: kernel parameters
 * @rx_cfg [in]: UDP receive configuration
 */
template <enum doca_gpu_dev_eth_exec_scope rx_scope>
static void launch_kernel_receive_udp(cudaStream_t stream,
				      uint16_t numq,
				      const struct udp_kernel_params &params,
				      const struct udp_rx_cfg *rx_cfg)
{
	if (rx_cfg->stage_latency && rx_cfg->process)
		launch_kernel_receive_udp_attr<rx_scope, true, true>(stream, numq, params, rx_cfg);
	else if (rx_cfg->stage_latency)
		launch_kernel_receive_udp_attr<rx_scope, true, false>(stream, numq, params, rx_cfg);
	else if (rx_cfg->process)
		launch_kernel_receive_udp_attr<rx_scope, false, true>(stream, numq, params, rx_cfg);
	else
		launch_kernel_receive_udp_attr<rx_scope, false, false>(stream, numq, params, rx_cfg);
}

extern "C" {

doca_error_t kernel_receive_udp(cudaStream_t stream,
				uint32_t *exit_cond,
				struct rxq_udp_queues *udp_queues,
				const struct udp_rx_cfg *rx_cfg)
{
	cudaError_t result = cudaSuccess;
	struct udp_kernel_params params = {0};

	if (udp_queues == NULL || udp_queues->numq == 0 || udp_queues->numq > MAX_QUEUES || exit_cond == 0 ||
	    rx_cfg == NULL) {
		DOCA_LOG_ERR("kernel_receive_udp invalid input values");
		return DOCA_ERROR_INVALID_VALUE;
	}

	if (rx_cfg->cqe_attr_check && udp_queues->rx_attr_gpu[0] == NULL) {
		DOCA_LOG_ERR(
			"kernel_receive_udp CQE attribute check requires UDP queues created with CQE attributes memory");
		return DOCA_ERROR_INVALID_VALUE;
	}

	/* Check no previous CUDA errors */
	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	params.exit_cond = exit_cond;
	for (int idx = 0; idx < udp_queues->numq; idx++) {
		params.rxq[idx] = udp_queues->eth_rxq_gpu[idx];
		params.sem[idx] = udp_queues->sem_gpu[idx];
		params.rx_attr[idx] = (struct doca_gpu_dev_eth_rxq_attr *)udp_queues->rx_attr_gpu[idx];
	}
	params.sem_num = udp_queues->nums;
	params.max_rx_pkts = rx_cfg->max_rx_pkts;
	params.timeout_ns = rx_cfg->timeout_ns;

	switch (rx_cfg->scope) {
	case DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD:
		launch_kernel_receive_udp<DOCA_GPUNETIO_ETH_EXEC_SCOPE_THREAD>(stream, udp_queues->numq, params, rx_cfg);
		break;
	case DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP:
		launch_kernel_receive_udp<DOCA_GPUNETIO_ETH_EXEC_SCOPE_WARP>(stream, udp_queues->numq, params, rx_cfg);
		break;
	case DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK:
		launch_kernel_receive_udp<DOCA_GPUNETIO_ETH_EXEC_SCOPE_BLOCK>(stream, udp_queues->numq, params, rx_cfg);
		break;
	default:
		DOCA_LOG_ERR("kernel_receive_udp invalid receive scope %d", rx_cfg->scope);
		return DOCA_ERROR_INVALID_VALUE;
	}

	result = cudaGetLastError();
	if (cudaSuccess != result) {
		DOCA_LOG_ERR("[%s:%d] cuda failed with %s \n", __FILE__, __LINE__, cudaGetErrorString(result));
		return DOCA_ERROR_BAD_STATE;
	}

	return DOCA_SUCCESS;
}

} /* extern C */
