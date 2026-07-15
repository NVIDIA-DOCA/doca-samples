#
# Copyright (c) 2022-2024 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

json_c_dependency = dependency('json-c', required: false)
if not json_c_dependency.found()
	warning('Skipping compilation of DOCA Application - @0@ - Missing json-c support.'.format(APP_NAME))
	subdir_done()
endif

app_dependencies += json_c_dependency

app_srcs += files([
	'config.c',
	'flow_common.c',
	'flow_decrypt.c',
	'flow_encrypt.c',
	'flow_encrypt.c',
	'ipsec_ctx.c',
	'policy.c',
	common_dir_path + '/dpdk_utils.c',
	common_dir_path + '/pack.c',
	common_dir_path + '/utils.c',
	common_dir_path + '/flow_parser.c',
	samples_dir_path + '/common.c',
])

vanilla_app_srcs = [
	APP_NAME + '.c',
]

executable(DOCA_PREFIX + APP_NAME,
	app_srcs + vanilla_app_srcs,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: install_apps)
