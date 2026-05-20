#
# Copyright (c) 2023-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright notice, this list of
#       conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright notice, this list of
#       conditions and the following disclaimer in the documentation and/or other materials
#       provided with the distribution.
#     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
#       to endorse or promote products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

app_srcs = files([
	'gpu_packet_processing.c',
	common_dir_path + '/utils.c',
	samples_dir_path + '/common.c',
])

config_srcs = files([
	'config_queues/args.c',
	'config_queues/device.c',
	'config_queues/flow.c',
	'config_queues/http_txbuf.c',
	'config_queues/udp_queues.c',
	'config_queues/tcp_queues.c',
	'config_queues/icmp_queues.c',
])

dpdk_srcs = files([
	'tcp_cpu/tcp_cpu_rss_func.c',
	'tcp_cpu/tcp_session_table.c',
])

kernels_srcs = files([
	'gpu_kernels/receive_tcp.cu',
	'gpu_kernels/receive_udp.cu',
	'gpu_kernels/receive_icmp.cu',
	'gpu_kernels/http_server.cu',
])

app_inc_dirs += include_directories('config_queues')

# Check if we should skip the compilation of the applications
if flag_disable_applications_compilation
	subdir_done()
endif

app_dependencies += gpu_dependencies

# GPUNetIO-based samples & apps are only supported in amalgamation build
if not flag_amalgamation_build
	set_variable('app_' + APP_NAME + '_disable_reason', 'Internal compilation isn\'t supported for this App outside of amalgamation mode')
	subdir_done()
endif

executable(DOCA_PREFIX + APP_NAME, app_srcs + config_srcs + dpdk_srcs + kernels_srcs,
	dependencies : app_dependencies + gpunetio_device_dep,
	# RM#3857532: Required for maintaining build order
	link_with : gpunetio_device_lib,
	c_args : apps_common_c_args + gpu_compile_flags,
	cuda_args : gpu_compile_flags,
	override_options : ['cpp_std=c++17'],
	include_directories: app_inc_dirs)
