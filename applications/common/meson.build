#
# Copyright (c) 2021-2025 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
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

common_dependencies  = []

# For the samples
apps_common_dir_include += doca_root_dir_include
# For us
apps_common_dir_include += include_directories('.')

apps_common_c_args = base_c_args
apps_common_cpp_args = base_cpp_args

if is_compiler_gcc or is_compiler_clang
	extra_flags =  [
		# From public meson config
		'-Wno-missing-braces',
		'-Wno-missing-field-initializers',
		# Self defined extra compiler checks
		'-Wunused-parameter',
		'-Wsign-compare',
	]
	apps_common_c_args += extra_flags
	apps_common_cpp_args += extra_flags

endif

if flag_enable_fault_injection
	apps_common_c_args += ['-include', 'priv_extern_doca_malloc.h']
	apps_common_cpp_args += ['-include', 'priv_extern_doca_malloc.h']
endif

common_files = [
	'comch_utils',
	'dpdk_utils',
	'pack',
	'packet_parser',
	'utils',
	'telemetry_exporter',
	'flow_parser',
	'flow_pipes_manager',
	'flow_skeleton',
]

common_srcs = []
common_hdrs = []
foreach cur_file : common_files
	common_srcs += [cur_file + '.c']
	common_hdrs += [cur_file + '.h']
endforeach

install_srcs = files(common_srcs)
install_hdrs = files(common_hdrs)

subdir('device')

install_data(sources : install_srcs + install_hdrs, install_dir : '/'.join([doca_applications_dir, 'common']))
# Provide verification with an API to this variable
install_app_common_srcs = install_srcs
