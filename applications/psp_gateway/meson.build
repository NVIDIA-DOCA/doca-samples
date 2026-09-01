#
# Copyright (c) 2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

if not flag_enable_grpc_support
	warning('Skipping compilation of DOCA Application - @0@ - Missing gRPC support.'.format(APP_NAME))
	subdir_done()
endif

json_c_dependency = dependency('json-c', required: false)
if not json_c_dependency.found()
	warning('Skipping compilation of DOCA Application - @0@ - Missing json-c support.'.format(APP_NAME))
	subdir_done()
endif

subdir('grpc')

app_dependencies += dependency_psp_gateway_grpc
app_dependencies += grpc_dependencies
app_dependencies += json_c_dependency

app_srcs = files([
	'psp_gw_flows.cpp',
	'psp_gw_params.cpp',
	'psp_gw_svc_impl.cpp',
	'psp_gw_pkt_rss.cpp',
	'psp_gw_utils.cpp',
	common_dir_path + '/dpdk_utils.c',
	samples_dir_path + '/common.c',
])

vanilla_app_srcs = [
	APP_NAME + '.cpp',
]

executable(DOCA_PREFIX + APP_NAME,
	app_srcs + vanilla_app_srcs,
	override_options: ['cpp_std=c++17'],
	c_args : grpc_c_args,
	cpp_args: grpc_cpp_args,
	link_args : grpc_link_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: install_apps)
