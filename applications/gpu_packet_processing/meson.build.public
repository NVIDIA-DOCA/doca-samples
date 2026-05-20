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

if not flag_enable_gpu_support
	warning('Skipping compilation of DOCA Application - @0@ - Missing GPU support.'.format(APP_NAME))
	subdir_done()
endif

app_dependencies += declare_dependency(link_args : '-lresolv')
app_dependencies += gpu_dependencies

doca_gpu_dep = dependency('doca-gpunetio')
gpunetio_device_path = doca_gpu_dep.get_variable(pkgconfig : 'libdir')
dependency_gpunetio_device = declare_dependency(compile_args : '-Wl,--whole-archive',
                                                link_args : ['-L' + gpunetio_device_path , '-ldoca_gpunetio_device'],)
app_dependencies += doca_gpu_dep
app_dependencies += dependency_gpunetio_device

app_srcs = files([
	'gpu_packet_processing.c',
	'config_queues/args.c',
	'config_queues/device.c',
	'config_queues/flow.c',
	'config_queues/http_txbuf.c',
	'config_queues/udp_queues.c',
	'config_queues/tcp_queues.c',
	'config_queues/icmp_queues.c',
	'tcp_cpu/tcp_cpu_rss_func.c',
	'tcp_cpu/tcp_session_table.c',
])

app_srcs += [
	common_dir_path + '/utils.c',
	samples_dir_path + '/common.c',
]

app_srcs += files([
	'gpu_kernels/receive_tcp.cu',
	'gpu_kernels/receive_udp.cu',
	'gpu_kernels/receive_icmp.cu',
	'gpu_kernels/http_server.cu',
])

vanilla_app_srcs = [
	APP_NAME + '.c',
]

app_inc_dirs += include_directories('config_queues')

executable(DOCA_PREFIX + APP_NAME,
		app_srcs + vanilla_app_srcs,
		c_args : gpu_c_args,
		cuda_args : gpu_cuda_args,
		dependencies : app_dependencies,
		include_directories : app_inc_dirs,
		install: install_apps)
